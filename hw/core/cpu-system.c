/*
 * QEMU CPU model (system specific)
 *
 * Copyright (c) 2012-2014 SUSE LINUX Products GmbH
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see
 * <http://www.gnu.org/licenses/gpl-2.0.html>
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "exec/tswap.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "hw/core/sysemu-cpu-ops.h"

bool cpu_paging_enabled(const CPUState *cpu)
{
    CPUClass *cc = CPU_GET_CLASS(cpu);

    if (cc->sysemu_ops->get_paging_enabled) {
        return cc->sysemu_ops->get_paging_enabled(cpu);
    }

    return false;
}

bool cpu_get_memory_mapping(CPUState *cpu, MemoryMappingList *list,
                            Error **errp)
{
    CPUClass *cc = CPU_GET_CLASS(cpu);

    if (cc->sysemu_ops->get_memory_mapping) {
        return cc->sysemu_ops->get_memory_mapping(cpu, list, errp);
    }

    error_setg(errp, "Obtaining memory mappings is unsupported on this CPU.");
    return false;
}

hwaddr cpu_get_phys_page_attrs_debug(CPUState *cpu, vaddr addr,
                                     MemTxAttrs *attrs)
{
    CPUClass *cc = CPU_GET_CLASS(cpu);
    hwaddr paddr;

    if (cc->sysemu_ops->get_phys_page_attrs_debug) {
        paddr = cc->sysemu_ops->get_phys_page_attrs_debug(cpu, addr, attrs);
    } else {
        /* Fallback for CPUs which don't implement the _attrs_ hook */
        *attrs = MEMTXATTRS_UNSPECIFIED;
        paddr = cc->sysemu_ops->get_phys_page_debug(cpu, addr);
    }
    /* Indicate that this is a debug access. */
    attrs->debug = 1;
    return paddr;
}

hwaddr cpu_get_phys_page_debug(CPUState *cpu, vaddr addr)
{
    MemTxAttrs attrs = {};

    return cpu_get_phys_page_attrs_debug(cpu, addr, &attrs);
}

int cpu_asidx_from_attrs(CPUState *cpu, MemTxAttrs attrs)
{
    int ret = 0;

    if (cpu->cc->sysemu_ops->asidx_from_attrs) {
        ret = cpu->cc->sysemu_ops->asidx_from_attrs(cpu, attrs);
        assert(ret < cpu->num_ases && ret >= 0);
    }
    return ret;
}

int cpu_write_elf32_qemunote(WriteCoreDumpFunction f, CPUState *cpu,
                             void *opaque)
{
    CPUClass *cc = CPU_GET_CLASS(cpu);

    if (!cc->sysemu_ops->write_elf32_qemunote) {
        return 0;
    }
    return (*cc->sysemu_ops->write_elf32_qemunote)(f, cpu, opaque);
}

int cpu_write_elf32_note(WriteCoreDumpFunction f, CPUState *cpu,
                         int cpuid, void *opaque)
{
    CPUClass *cc = CPU_GET_CLASS(cpu);

    if (!cc->sysemu_ops->write_elf32_note) {
        return -1;
    }
    return (*cc->sysemu_ops->write_elf32_note)(f, cpu, cpuid, opaque);
}

int cpu_write_elf64_qemunote(WriteCoreDumpFunction f, CPUState *cpu,
                             void *opaque)
{
    CPUClass *cc = CPU_GET_CLASS(cpu);

    if (!cc->sysemu_ops->write_elf64_qemunote) {
        return 0;
    }
    return (*cc->sysemu_ops->write_elf64_qemunote)(f, cpu, opaque);
}

int cpu_write_elf64_note(WriteCoreDumpFunction f, CPUState *cpu,
                         int cpuid, void *opaque)
{
    CPUClass *cc = CPU_GET_CLASS(cpu);

    if (!cc->sysemu_ops->write_elf64_note) {
        return -1;
    }
    return (*cc->sysemu_ops->write_elf64_note)(f, cpu, cpuid, opaque);
}

bool cpu_virtio_is_big_endian(CPUState *cpu)
{
    CPUClass *cc = CPU_GET_CLASS(cpu);

    if (cc->sysemu_ops->virtio_is_big_endian) {
        return cc->sysemu_ops->virtio_is_big_endian(cpu);
    }
    return target_words_bigendian();
}

GuestPanicInformation *cpu_get_crash_info(CPUState *cpu)
{
    CPUClass *cc = CPU_GET_CLASS(cpu);
    GuestPanicInformation *res = NULL;

    if (cc->sysemu_ops->get_crash_info) {
        res = cc->sysemu_ops->get_crash_info(cpu);
    }
    return res;
}

static const Property cpu_system_props[] = {
    /*
     * Create a memory property for system CPU object, so users can
     * wire up its memory.  The default if no link is set up is to use
     * the system address space.
     */
    DEFINE_PROP_LINK("memory", CPUState, memory, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

static bool cpu_get_start_powered_off(Object *obj, Error **errp)
{
    CPUState *cpu = CPU(obj);
    return cpu->start_powered_off;
}

static void cpu_set_start_powered_off(Object *obj, bool value, Error **errp)
{
    CPUState *cpu = CPU(obj);
    cpu->start_powered_off = value;
}

void cpu_class_init_props(DeviceClass *dc)
{
    ObjectClass *oc = OBJECT_CLASS(dc);

    /*
     * We can't use DEFINE_PROP_BOOL in the Property array for this
     * property, because we want this to be settable after realize.
     */
    object_class_property_add_bool(oc, "start-powered-off",
                                   cpu_get_start_powered_off,
                                   cpu_set_start_powered_off);

    device_class_set_props(dc, cpu_system_props);
}

void cpu_exec_initfn(CPUState *cpu)
{
    cpu->memory = get_system_memory();
    object_ref(OBJECT(cpu->memory));
}
