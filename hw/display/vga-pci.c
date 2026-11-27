/*
 * QEMU PCI VGA Emulator.
 *
 * see docs/specs/standard-vga.txt for virtual hardware specs.
 *
 * Copyright (c) 2003 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "vga_int.h"
#include "vga_regs.h"
#include "ui/pixel_ops.h"
#include "ui/console.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/replay.h"
#include "sysemu/tcg.h"
#include "hw/loader.h"
#include "hw/display/edid.h"
#include "qom/object.h"
#include "hw/acpi/acpi_aml_interface.h"

enum vga_pci_flags {
    PCI_VGA_FLAG_ENABLE_MMIO = 1,
    PCI_VGA_FLAG_ENABLE_QEXT = 2,
    PCI_VGA_FLAG_ENABLE_EDID = 3,
};

struct PCIVGAState {
    PCIDevice dev;
    VGACommonState vga;
    uint32_t flags;
    qemu_edid_info edid_info;
    MemoryRegion mmio;
    MemoryRegion mrs[4];
    uint8_t edid[384];
};

#define TYPE_PCI_VGA "pci-vga"
OBJECT_DECLARE_SIMPLE_TYPE(PCIVGAState, PCI_VGA)

static const VMStateDescription vmstate_vga_pci = {
    .name = "vga",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, PCIVGAState),
        VMSTATE_STRUCT(vga, PCIVGAState, 0, vmstate_vga_common, VGACommonState),
        VMSTATE_END_OF_LIST()
    }
};

static QemuRecMutex *pci_vga_dac_split_lock(void *opaque, hwaddr addr,
                                           unsigned size)
{
    VGACommonState *s = opaque;
    hwaddr port = addr + 0x3c0;

    if (s->dac_local && port <= VGA_PEL_D && port + size > VGA_PEL_IR) {
        return &s->dac_lock;
    }
    return NULL;
}

static VGADACLock *pci_vga_lock_dac(VGACommonState *s, hwaddr addr,
                                    unsigned size)
{
    QemuRecMutex *lock = pci_vga_dac_split_lock(s, addr, size);

    if (lock) {
        qemu_rec_mutex_lock(lock);
    }
    return lock;
}

static uint64_t pci_vga_ioport_read(void *ptr, hwaddr addr, unsigned size)
{
    VGACommonState *s = ptr;
    g_autoptr(VGADACLock) lock = pci_vga_lock_dac(s, addr, size);
    uint64_t value = 0;

    for (unsigned offset = 0; offset < size; offset++) {
        if (addr + 0x3c0 + offset == 0x3d4 && offset + 1 < size) {
            value |= (uint64_t)vga_ioport_readw(s, 0x3d4) << (8 * offset);
            offset++;
        } else {
            value |= (uint64_t)vga_ioport_read(s, addr + 0x3c0 + offset)
                     << (8 * offset);
        }
    }
    return value;
}

static void pci_vga_ioport_write(void *ptr, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    VGACommonState *s = ptr;
    g_autoptr(VGADACLock) lock = pci_vga_lock_dac(s, addr, size);

    /* Write the index byte before the data byte. */
    for (unsigned offset = 0; offset < size; offset++) {
        if (addr + 0x3c0 + offset == 0x3d4 && offset + 1 < size) {
            vga_ioport_writew(s, 0x3d4, (val >> (8 * offset)) & 0xffff);
            offset++;
        } else {
            vga_ioport_write(s, addr + 0x3c0 + offset, (val >> (8 * offset)) & 0xff);
        }
    }
}

#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
static QemuRecMutex *pci_vga_crtc_split_lock(void *opaque, hwaddr addr,
                                            unsigned size)
{
    VGACommonState *s = opaque;
    unsigned index = VGA_CRT_IC - VGA_ATT_W;

    /*
     * This bank only accesses CRT and status registers. A transfer starting
     * at or before the graphics controller can change the memory map and must
     * use the pause callback without holding the VGA lock.
     */
    if (!vga_has_local_lock(s) || addr <= VGA_GFX_D - VGA_ATT_W ||
        size > PCI_VGA_IOPORT_SIZE - addr) {
        return NULL;
    }
    if (s->local_ports || (addr <= index && size > index + 1 - addr)) {
        return &s->lowmem_lock;
    }
    return NULL;
}
#endif

static const MemoryRegionOps pci_vga_ioport_ops = {
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    .mmio_split_lock = pci_vga_crtc_split_lock,
    .mmio_split_pause = vga_pause_remapped_crtc,
    .mmio_split_resume = vga_resume_remapped_crtc,
#endif
    .read = pci_vga_ioport_read,
    .write = pci_vga_ioport_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static QemuRecMutex *pci_vga_register_split_lock(void *opaque, hwaddr addr,
                                                 unsigned size)
{
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    QemuRecMutex *crtc = pci_vga_crtc_split_lock(opaque, addr, size);
    if (crtc) {
        return crtc;
    }
#endif
    return pci_vga_dac_split_lock(opaque, addr, size);
}

static const MemoryRegionOps pci_vga_ioport_local_dac_ops = {
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    .mmio_split_pause = vga_pause_remapped_crtc,
    .mmio_split_resume = vga_resume_remapped_crtc,
#endif
    .read = pci_vga_ioport_read,
    .write = pci_vga_ioport_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .mmio_split_lock = pci_vga_register_split_lock,
};

static uint64_t pci_vga_bochs_read(void *ptr, hwaddr addr,
                                   unsigned size)
{
    VGACommonState *s = ptr;
    g_autoptr(VGALowmemLock) lock = vga_lock_lowmem(s);
    uint64_t value = 0;

    for (unsigned offset = 0; offset < size; offset += 2) {
        vbe_ioport_write_index(s, 0, (addr + offset) >> 1);
        value |= (uint64_t)(uint16_t)vbe_ioport_read_data(s, 0) << (offset * 8);
    }
    return value;
}

static void pci_vga_bochs_write(void *ptr, hwaddr addr,
                                uint64_t val, unsigned size)
{
    vbe_write_registers(ptr, addr >> 1, val, size);
}

static const MemoryRegionOps pci_vga_bochs_ops = {
    .read = pci_vga_bochs_read,
    .write = pci_vga_bochs_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 2,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t pci_vga_qext_read(void *ptr, hwaddr addr, unsigned size)
{
    VGACommonState *s = ptr;

    switch (addr) {
    case PCI_VGA_QEXT_REG_SIZE:
        return PCI_VGA_QEXT_SIZE;
    case PCI_VGA_QEXT_REG_BYTEORDER:
        return s->big_endian_fb ?
            PCI_VGA_QEXT_BIG_ENDIAN : PCI_VGA_QEXT_LITTLE_ENDIAN;
    default:
        return 0;
    }
}

static void pci_vga_qext_write(void *ptr, hwaddr addr,
                               uint64_t val, unsigned size)
{
    VGACommonState *s = ptr;

    switch (addr) {
    case PCI_VGA_QEXT_REG_BYTEORDER:
        if (val == PCI_VGA_QEXT_BIG_ENDIAN) {
            vga_scanout_changed(s);
            s->big_endian_fb = true;
        }
        if (val == PCI_VGA_QEXT_LITTLE_ENDIAN) {
            vga_scanout_changed(s);
            s->big_endian_fb = false;
        }
        break;
    }
}

static bool vga_get_big_endian_fb(Object *obj, Error **errp)
{
    PCIVGAState *d = PCI_VGA(PCI_DEVICE(obj));

    return d->vga.big_endian_fb;
}

static void vga_set_big_endian_fb(Object *obj, bool value, Error **errp)
{
    PCIVGAState *d = PCI_VGA(PCI_DEVICE(obj));

    vga_scanout_changed(&d->vga);
    d->vga.big_endian_fb = value;
}

static const MemoryRegionOps pci_vga_qext_ops = {
    .read = pci_vga_qext_read,
    .write = pci_vga_qext_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

void pci_std_vga_mmio_region_init(VGACommonState *s,
                                  Object *owner,
                                  MemoryRegion *parent,
                                  MemoryRegion *subs,
                                  bool qext, bool edid)
{
    PCIVGAState *d = container_of(s, PCIVGAState, vga);

    const MemoryRegionOps *io_ops = s->dac_local ?
        &pci_vga_ioport_local_dac_ops : &pci_vga_ioport_ops;

    memory_region_init_io(&subs[0], owner, io_ops, s,
                          "vga ioports remapped", PCI_VGA_IOPORT_SIZE);
    memory_region_add_subregion(parent, PCI_VGA_IOPORT_OFFSET,
                                &subs[0]);

    memory_region_init_io(&subs[1], owner, &pci_vga_bochs_ops, s,
                          "bochs dispi interface", PCI_VGA_BOCHS_SIZE);
    memory_region_add_subregion(parent, PCI_VGA_BOCHS_OFFSET,
                                &subs[1]);

    if (qext) {
        memory_region_init_io(&subs[2], owner, &pci_vga_qext_ops, s,
                              "qemu extended regs", PCI_VGA_QEXT_SIZE);
        memory_region_add_subregion(parent, PCI_VGA_QEXT_OFFSET,
                                    &subs[2]);
    }

    if (edid) {
        qemu_edid_generate(d->edid, sizeof(d->edid), &d->edid_info);
        qemu_edid_region_io(&subs[3], owner, d->edid, sizeof(d->edid));
        memory_region_add_subregion(parent, 0, &subs[3]);
    }
}

static void pci_std_vga_realize(PCIDevice *dev, Error **errp)
{
    PCIVGAState *d = PCI_VGA(dev);
    VGACommonState *s = &d->vga;
    bool qext = false;
    bool edid = false;

    /* vga + console init */
    if (!vga_common_init(s, OBJECT(dev), errp)) {
        return;
    }
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    if (tcg_enabled() && !icount_enabled() &&
        replay_mode == REPLAY_MODE_NONE) {
        s->lowmem_local =
            lats_nobql_enabled();
        s->local_ports =
            lats_nobql_enabled();
    }
    if (vga_has_local_lock(s)) {
        qemu_rec_mutex_init(&s->lowmem_lock);
    }
    s->dac_local = tcg_enabled() && !icount_enabled() &&
        replay_mode == REPLAY_MODE_NONE &&
        lats_nobql_enabled();
    if (s->dac_local) {
        qemu_rec_mutex_init(&s->dac_lock);
    }
    s->scanout_notify = tcg_enabled() && !icount_enabled() &&
        replay_mode == REPLAY_MODE_NONE &&
        lats_nobql_enabled();
#endif
    vga_init(s, OBJECT(dev), pci_address_space(dev), pci_address_space_io(dev),
             true);

    s->con = graphic_console_init(DEVICE(dev), 0, s->hw_ops, s);

    /* XXX: VGA_RAM_SIZE must be a power of two */
    pci_register_bar(&d->dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &s->vram);

    /* mmio bar for vga register access */
    if (d->flags & (1 << PCI_VGA_FLAG_ENABLE_MMIO)) {
        memory_region_init_io(&d->mmio, OBJECT(dev), &unassigned_io_ops, NULL,
                              "vga.mmio", PCI_VGA_MMIO_SIZE);

        if (d->flags & (1 << PCI_VGA_FLAG_ENABLE_QEXT)) {
            qext = true;
            pci_set_byte(&d->dev.config[PCI_REVISION_ID], 2);
        }
        if (d->flags & (1 << PCI_VGA_FLAG_ENABLE_EDID)) {
            edid = true;
        }
        pci_std_vga_mmio_region_init(s, OBJECT(dev), &d->mmio, d->mrs,
                                     qext, edid);

        pci_register_bar(&d->dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->mmio);
    }
}

static void pci_secondary_vga_realize(PCIDevice *dev, Error **errp)
{
    PCIVGAState *d = PCI_VGA(dev);
    VGACommonState *s = &d->vga;
    bool qext = false;
    bool edid = false;

    /* vga + console init */
    if (!vga_common_init(s, OBJECT(dev), errp)) {
        return;
    }
    s->con = graphic_console_init(DEVICE(dev), 0, s->hw_ops, s);

    /* mmio bar */
    memory_region_init_io(&d->mmio, OBJECT(dev), &unassigned_io_ops, NULL,
                          "vga.mmio", PCI_VGA_MMIO_SIZE);

    if (d->flags & (1 << PCI_VGA_FLAG_ENABLE_QEXT)) {
        qext = true;
        pci_set_byte(&d->dev.config[PCI_REVISION_ID], 2);
    }
    if (d->flags & (1 << PCI_VGA_FLAG_ENABLE_EDID)) {
        edid = true;
    }
    pci_std_vga_mmio_region_init(s, OBJECT(dev), &d->mmio, d->mrs, qext, edid);

    pci_register_bar(&d->dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &s->vram);
    pci_register_bar(&d->dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->mmio);
}

static void pci_secondary_vga_exit(PCIDevice *dev)
{
    PCIVGAState *d = PCI_VGA(dev);
    VGACommonState *s = &d->vga;

    graphic_console_close(s->con);
    memory_region_del_subregion(&d->mmio, &d->mrs[0]);
    memory_region_del_subregion(&d->mmio, &d->mrs[1]);
    if (d->flags & (1 << PCI_VGA_FLAG_ENABLE_QEXT)) {
        memory_region_del_subregion(&d->mmio, &d->mrs[2]);
    }
    if (d->flags & (1 << PCI_VGA_FLAG_ENABLE_EDID)) {
        memory_region_del_subregion(&d->mmio, &d->mrs[3]);
    }
}

static void pci_secondary_vga_init(Object *obj)
{
    /* Expose framebuffer byteorder via QOM */
    object_property_add_bool(obj, "big-endian-framebuffer",
                             vga_get_big_endian_fb, vga_set_big_endian_fb);
}

static void pci_secondary_vga_reset(DeviceState *dev)
{
    PCIVGAState *d = PCI_VGA(PCI_DEVICE(dev));
    vga_common_reset(&d->vga);
}

static Property vga_pci_properties[] = {
    DEFINE_PROP_UINT32("vgamem_mb", PCIVGAState, vga.vram_size_mb, 16),
    DEFINE_PROP_BIT("mmio", PCIVGAState, flags, PCI_VGA_FLAG_ENABLE_MMIO, true),
    DEFINE_PROP_BIT("qemu-extended-regs",
                    PCIVGAState, flags, PCI_VGA_FLAG_ENABLE_QEXT, true),
    DEFINE_PROP_BIT("edid",
                    PCIVGAState, flags, PCI_VGA_FLAG_ENABLE_EDID, true),
    DEFINE_EDID_PROPERTIES(PCIVGAState, edid_info),
    DEFINE_PROP_BOOL("global-vmstate", PCIVGAState, vga.global_vmstate, false),
    DEFINE_PROP_END_OF_LIST(),
};

static Property secondary_pci_properties[] = {
    DEFINE_PROP_UINT32("vgamem_mb", PCIVGAState, vga.vram_size_mb, 16),
    DEFINE_PROP_BIT("qemu-extended-regs",
                    PCIVGAState, flags, PCI_VGA_FLAG_ENABLE_QEXT, true),
    DEFINE_PROP_BIT("edid",
                    PCIVGAState, flags, PCI_VGA_FLAG_ENABLE_EDID, true),
    DEFINE_EDID_PROPERTIES(PCIVGAState, edid_info),
    DEFINE_PROP_END_OF_LIST(),
};

static void vga_pci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    AcpiDevAmlIfClass *adevc = ACPI_DEV_AML_IF_CLASS(klass);

    k->vendor_id = PCI_VENDOR_ID_QEMU;
    k->device_id = PCI_DEVICE_ID_QEMU_VGA;
    dc->vmsd = &vmstate_vga_pci;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    adevc->build_dev_aml = build_vga_aml;
}

static const TypeInfo vga_pci_type_info = {
    .name = TYPE_PCI_VGA,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIVGAState),
    .abstract = true,
    .class_init = vga_pci_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { TYPE_ACPI_DEV_AML_IF },
        { },
    },
};

static void vga_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->plain_config_header = true;
    k->plain_config_space = true;
    k->realize = pci_std_vga_realize;
    k->romfile = "vgabios-stdvga.bin";
    k->class_id = PCI_CLASS_DISPLAY_VGA;
    device_class_set_props(dc, vga_pci_properties);
    dc->hotpluggable = false;

    /* Expose framebuffer byteorder via QOM */
    object_class_property_add_bool(klass, "big-endian-framebuffer",
                                   vga_get_big_endian_fb, vga_set_big_endian_fb);
}

static void secondary_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = pci_secondary_vga_realize;
    k->exit = pci_secondary_vga_exit;
    k->class_id = PCI_CLASS_DISPLAY_OTHER;
    device_class_set_props(dc, secondary_pci_properties);
    dc->reset = pci_secondary_vga_reset;
}

static void pci_std_vga_finalize(Object *obj)
{
    VGACommonState *s = &PCI_VGA(obj)->vga;

    if (s->dac_local) {
        qemu_rec_mutex_destroy(&s->dac_lock);
    }
    if (s->lowmem_local) {
        qemu_rec_mutex_destroy(&s->lowmem_lock);
    }
}

static const TypeInfo vga_info = {
    .name          = "VGA",
    .parent        = TYPE_PCI_VGA,
    .class_init    = vga_class_init,
    .instance_finalize = pci_std_vga_finalize,
};

static const TypeInfo secondary_info = {
    .name          = "secondary-vga",
    .parent        = TYPE_PCI_VGA,
    .instance_init = pci_secondary_vga_init,
    .class_init    = secondary_class_init,
};

static void vga_register_types(void)
{
    type_register_static(&vga_pci_type_info);
    type_register_static(&vga_info);
    type_register_static(&secondary_info);
}

type_init(vga_register_types)
