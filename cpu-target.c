/*
 * Target-specific parts of the CPU object
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/qemu-print.h"
#include "migration/vmstate.h"
#ifndef CONFIG_USER_ONLY
#include "hw/core/sysemu-cpu-ops.h"
#endif
#include "system/accel-ops.h"
#include "system/cpus.h"
#include "system/tcg.h"
#include "exec/tswap.h"
#include "exec/replay-core.h"
#include "exec/cpu-common.h"
#include "exec/cputlb.h"
#include "exec/exec-all.h"
#include "exec/tb-flush.h"
#include "exec/log.h"
#include "accel/accel-cpu-target.h"
#include "trace/trace-root.h"
#include "qemu/accel.h"
#include "hw/core/cpu.h"

#ifndef CONFIG_USER_ONLY
static int cpu_common_post_load(void *opaque, int version_id)
{
    if (tcg_enabled()) {
        CPUState *cpu = opaque;

        /*
         * 0x01 was CPU_INTERRUPT_EXIT. This line can be removed when the
         * version_id is increased.
         */
        cpu->interrupt_request &= ~0x01;

        tlb_flush(cpu);

        /*
         * loadvm has just updated the content of RAM, bypassing the
         * usual mechanisms that ensure we flush TBs for writes to
         * memory we've translated code from. So we must flush all TBs,
         * which will now be stale.
         */
        tb_flush(cpu);
    }

    return 0;
}

static int cpu_common_pre_load(void *opaque)
{
    CPUState *cpu = opaque;

    cpu->exception_index = -1;

    return 0;
}

static bool cpu_common_exception_index_needed(void *opaque)
{
    CPUState *cpu = opaque;

    return tcg_enabled() && cpu->exception_index != -1;
}

static const VMStateDescription vmstate_cpu_common_exception_index = {
    .name = "cpu_common/exception_index",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = cpu_common_exception_index_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(exception_index, CPUState),
        VMSTATE_END_OF_LIST()
    }
};

static bool cpu_common_crash_occurred_needed(void *opaque)
{
    CPUState *cpu = opaque;

    return cpu->crash_occurred;
}

static const VMStateDescription vmstate_cpu_common_crash_occurred = {
    .name = "cpu_common/crash_occurred",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = cpu_common_crash_occurred_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(crash_occurred, CPUState),
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription vmstate_cpu_common = {
    .name = "cpu_common",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = cpu_common_pre_load,
    .post_load = cpu_common_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(halted, CPUState),
        VMSTATE_UINT32(interrupt_request, CPUState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_cpu_common_exception_index,
        &vmstate_cpu_common_crash_occurred,
        NULL
    }
};
#endif

bool cpu_exec_realizefn(CPUState *cpu, Error **errp)
{
    if (!accel_cpu_common_realize(cpu, errp)) {
        return false;
    }

    /* Wait until cpu initialization complete before exposing cpu. */
    cpu_list_add(cpu);

#ifdef CONFIG_USER_ONLY
    assert(qdev_get_vmsd(DEVICE(cpu)) == NULL ||
           qdev_get_vmsd(DEVICE(cpu))->unmigratable);
#else
    if (qdev_get_vmsd(DEVICE(cpu)) == NULL) {
        vmstate_register(NULL, cpu->cpu_index, &vmstate_cpu_common, cpu);
    }
    if (cpu->cc->sysemu_ops->legacy_vmsd != NULL) {
        vmstate_register(NULL, cpu->cpu_index, cpu->cc->sysemu_ops->legacy_vmsd, cpu);
    }
#endif /* CONFIG_USER_ONLY */

    return true;
}

void cpu_exec_unrealizefn(CPUState *cpu)
{
#ifndef CONFIG_USER_ONLY
    CPUClass *cc = CPU_GET_CLASS(cpu);

    if (cc->sysemu_ops->legacy_vmsd != NULL) {
        vmstate_unregister(NULL, cc->sysemu_ops->legacy_vmsd, cpu);
    }
    if (qdev_get_vmsd(DEVICE(cpu)) == NULL) {
        vmstate_unregister(NULL, &vmstate_cpu_common, cpu);
    }
#endif

    cpu_list_remove(cpu);
    /*
     * Now that the vCPU has been removed from the RCU list, we can call
     * accel_cpu_common_unrealize, which may free fields using call_rcu.
     */
    accel_cpu_common_unrealize(cpu);
}

char *cpu_model_from_type(const char *typename)
{
    const char *suffix = "-" CPU_RESOLVING_TYPE;

    if (!object_class_by_name(typename)) {
        return NULL;
    }

    if (g_str_has_suffix(typename, suffix)) {
        return g_strndup(typename, strlen(typename) - strlen(suffix));
    }

    return g_strdup(typename);
}

const char *parse_cpu_option(const char *cpu_option)
{
    ObjectClass *oc;
    CPUClass *cc;
    gchar **model_pieces;
    const char *cpu_type;

    model_pieces = g_strsplit(cpu_option, ",", 2);
    if (!model_pieces[0]) {
        error_report("-cpu option cannot be empty");
        exit(1);
    }

    oc = cpu_class_by_name(CPU_RESOLVING_TYPE, model_pieces[0]);
    if (oc == NULL) {
        error_report("unable to find CPU model '%s'", model_pieces[0]);
        g_strfreev(model_pieces);
        exit(EXIT_FAILURE);
    }

    cpu_type = object_class_get_name(oc);
    cc = CPU_CLASS(oc);
    cc->parse_features(cpu_type, model_pieces[1], &error_fatal);
    g_strfreev(model_pieces);
    return cpu_type;
}

#ifndef cpu_list
static void cpu_list_entry(gpointer data, gpointer user_data)
{
    CPUClass *cc = CPU_CLASS(OBJECT_CLASS(data));
    const char *typename = object_class_get_name(OBJECT_CLASS(data));
    g_autofree char *model = cpu_model_from_type(typename);

    if (cc->deprecation_note) {
        qemu_printf("  %s (deprecated)\n", model);
    } else {
        qemu_printf("  %s\n", model);
    }
}

static void cpu_list(void)
{
    GSList *list;

    list = object_class_get_list_sorted(TYPE_CPU, false);
    qemu_printf("Available CPUs:\n");
    g_slist_foreach(list, cpu_list_entry, NULL);
    g_slist_free(list);
}
#endif

void list_cpus(void)
{
    cpu_list();
}

/* enable or disable single step mode. EXCP_DEBUG is returned by the
   CPU loop after each instruction */
void cpu_single_step(CPUState *cpu, int enabled)
{
    if (cpu->singlestep_enabled != enabled) {
        cpu->singlestep_enabled = enabled;

#if !defined(CONFIG_USER_ONLY)
        const AccelOpsClass *ops = cpus_get_accel();
        if (ops->update_guest_debug) {
            ops->update_guest_debug(cpu);
        }
#endif

        trace_breakpoint_singlestep(cpu->cpu_index, enabled);
    }
}

void cpu_abort(CPUState *cpu, const char *fmt, ...)
{
    va_list ap;
    va_list ap2;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    fprintf(stderr, "qemu: fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    cpu_dump_state(cpu, stderr, CPU_DUMP_FPU | CPU_DUMP_CCOP);
    if (qemu_log_separate()) {
        FILE *logfile = qemu_log_trylock();
        if (logfile) {
            fprintf(logfile, "qemu: fatal: ");
            vfprintf(logfile, fmt, ap2);
            fprintf(logfile, "\n");
            cpu_dump_state(cpu, logfile, CPU_DUMP_FPU | CPU_DUMP_CCOP);
            qemu_log_unlock(logfile);
        }
    }
    va_end(ap2);
    va_end(ap);
    replay_finish();
#if defined(CONFIG_USER_ONLY)
    {
        struct sigaction act;
        sigfillset(&act.sa_mask);
        act.sa_handler = SIG_DFL;
        act.sa_flags = 0;
        sigaction(SIGABRT, &act, NULL);
    }
#endif
    abort();
}

bool target_words_bigendian(void)
{
    return TARGET_BIG_ENDIAN;
}

const char *target_name(void)
{
    return TARGET_NAME;
}
