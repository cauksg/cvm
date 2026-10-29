/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Experimental M-only resource controller, not TDISP/IDE. Included by virt.c.
 * Firmware installs PMP before entering HS. */
#define COVE_CONTROL_BASE 0x03011000
#define COVE_CONTROL_MAGIC 0x43564d01

static void virt_cove_scrub_device(PCIDevice *dev)
{
    dev->assignment_scrub = true;
    device_cold_reset(&dev->qdev);
    dev->assignment_scrub = false;
    msi_reset(dev);
    msix_reset(dev);
    pci_default_write_config(dev, PCI_COMMAND, 0, 2);
}

static PCIDevice *virt_cove_selected(RISCVVirtState *s)
{
    return s->cove_pci_bus && s->cove_selected_rid > 0 &&
           s->cove_selected_rid < 64 ?
           pci_find_device(s->cove_pci_bus, 0, s->cove_selected_rid) : NULL;
}

static uint64_t virt_cove_bar_address(PCIDevice *dev, unsigned i)
{
    PCIIORegion *r = &dev->io_regions[i];
    unsigned off = i == PCI_ROM_SLOT ? PCI_ROM_ADDRESS :
                   PCI_BASE_ADDRESS_0 + i * 4;
    uint64_t address = pci_get_long(dev->config + off);

    if (!r->size) {
        return 0;
    }
    address &= ~(r->size - 1);
    return address;
}

static uint64_t virt_cove_control_read(void *opaque, hwaddr off, unsigned size)
{
    RISCVVirtState *s = opaque;
    PCIDevice *dev = virt_cove_selected(s);

    switch (off) {
    case 0: return COVE_CONTROL_MAGIC;
    case 8: return s->cove_selected_rid;
    case 0x18: return s->cove_control_status;
    case 0x128: return s->soc[0].harts[0].env.cove_rid_owner[s->cove_selected_rid];
    case 0x30: return s->soc[0].harts[0].env.cove_file_owner[s->soc[0].harts[0].env.cove_selected_file];
    case 0x38: return s->soc[0].harts[0].env.cove_selected_file;
    case 0x28: return s->soc[0].harts[0].env.cove_irq_iid[s->cove_selected_rid];
    default:
        if (dev && off >= 0x40 && off < 0x40 + PCI_NUM_REGIONS * 16) {
            PCIIORegion *r = &dev->io_regions[(off - 0x40) / 16];
            uint64_t address = virt_cove_bar_address(dev, (off - 0x40) / 16);
            if (!address) {
                return 0;
            }
            return (off & 8) ? r->size :
                address + ((r->type & PCI_BASE_ADDRESS_SPACE_IO) ?
                           s->memmap[VIRT_PCIE_PIO].base : 0);
        }
        return 0;
    }
}

static void virt_cove_control_write(void *opaque, hwaddr off, uint64_t val,
                                    unsigned size)
{
    RISCVVirtState *s = opaque;
    CPURISCVState *env = &s->soc[0].harts[0].env;
    PCIDevice *dev = virt_cove_selected(s);

    if (off == 8) {
        s->cove_selected_rid = val < 64 ? val : 0;
        return;
    }
    if (off == 0x20) {
        env->cove_guest_access = 0;
        for (unsigned file = 1; file < 8; file++) {
            if (val && env->cove_file_owner[file] == val) {
                env->cove_guest_access = val;
                break;
            }
        }
        tlb_flush(env_cpu(env));
        return;
    }
    if (off == 0x38) {
        if (val > 0 && val < 8) env->cove_selected_file = val;
        return;
    }
    if (off == 0x30) {
        env->cove_file_owner[env->cove_selected_file] = val;
        tlb_flush(env_cpu(env));
        return;
    }
    if (off == 0x28 && env->cove_claimed_rid[s->cove_selected_rid]) {
        unsigned rid = s->cove_selected_rid;
        if (!val || val >= 256) return;
        for (unsigned other = 1; other < 64; other++)
            if (other != rid && env->cove_irq_iid[other] == val &&
                env->cove_irq_file[other] == env->cove_selected_file) return;
        if (!env->cove_irq_iid[rid] || val == env->cove_irq_iid[rid]) {
            env->cove_irq_iid[rid] = val;
            env->cove_irq_file[rid] = env->cove_selected_file;
        }
        return;
    }
    if (off == 0x128) {
        unsigned rid = s->cove_selected_rid;
        if (env->cove_claimed_rid[rid] && val &&
            (!env->cove_rid_owner[rid] || env->cove_rid_owner[rid] == val)) {
            env->cove_rid_owner[rid] = val;
            tlb_flush(env_cpu(env));
        }
        return;
    }
    if (off != 0x10) {
        return;
    }
    s->cove_control_status = 1;
    unsigned rid = s->cove_selected_rid;
    if (!dev) {
        return;
    }
    if (val == 1 && !env->cove_claimed_rid[rid]) {
        PCIIORegion *bar0 = &dev->io_regions[0];
        uint64_t address = virt_cove_bar_address(dev, 0);
        if (address < MBMC_MMIO_BASE ||
            address >= MBMC_MMIO_END || !bar0->size ||
            bar0->size > MBMC_MMIO_END - address) {
            return;
        }
        for (unsigned i = 0; i < PCI_NUM_REGIONS; i++) {
            PCIIORegion *r = &dev->io_regions[i];
            address = virt_cove_bar_address(dev, i);
            /* Unmapped resources (normally the optional expansion ROM)
             * have no CPU alias. Firmware freezes the whole configuration
             * header, including ROM decode, for the complete claim lifetime. */
            if (!r->size || !address) {
                continue;
            }
            if (!(r->type & PCI_BASE_ADDRESS_SPACE_IO) &&
                ((address | r->size) & 4095)) {
                return;
            }
            /* No endpoint may shadow a protected page after Guest START. */
            for (unsigned other_rid = 1; other_rid < 64; other_rid++) {
                PCIDevice *other = pci_find_device(s->cove_pci_bus, 0, other_rid);
                if (!other || other == dev) {
                    continue;
                }
                for (unsigned j = 0; j < PCI_NUM_REGIONS; j++) {
                    PCIIORegion *q = &other->io_regions[j];
                    uint64_t alias = virt_cove_bar_address(other, j);
                    if (!alias || !q->size ||
                        ((q->type ^ r->type) & PCI_BASE_ADDRESS_SPACE_IO)) {
                        continue;
                    }
                    if ((address & ~UINT64_C(4095)) <
                        ROUND_UP(alias + q->size, 4096) &&
                        (alias & ~UINT64_C(4095)) <
                        ROUND_UP(address + r->size, 4096)) {
                        return;
                    }
                }
            }
        }
        for (unsigned i = 0; i < PCI_NUM_REGIONS; i++) {
            PCIIORegion *r = &dev->io_regions[i];
            address = virt_cove_bar_address(dev, i);
            env->cove_bar_size[rid][i] = address ? r->size : 0;
            env->cove_bar_addr[rid][i] = address +
                ((r->type & PCI_BASE_ADDRESS_SPACE_IO) ?
                 s->memmap[VIRT_PCIE_PIO].base : 0);
            /* TLB permissions cover an entire 4K page. Include PIO padding
             * so warming a neighboring register cannot bypass the guard. */
            if (env->cove_bar_size[rid][i]) {
                uint64_t end = env->cove_bar_addr[rid][i] + env->cove_bar_size[rid][i];
                env->cove_bar_addr[rid][i] &= ~UINT64_C(4095);
                env->cove_bar_size[rid][i] = ROUND_UP(end, 4096) -
                                        env->cove_bar_addr[rid][i];
            }
        }
        env->cove_claimed_rid[rid] = rid + 1;
        env->cove_rid_owner[rid] = 0;
        env->cove_guest_access = false;
        /* Eliminate Host-prearmed descriptors/timers while IOMMU context is
         * still invalid. Guest START must never resume an old Host command. */
        virt_cove_scrub_device(dev);
    } else if ((val == 2 || val == 3) && env->cove_claimed_rid[rid]) {
        /* DMA/MSI already drained by firmware. Preserve BAR addresses. */
        virt_cove_scrub_device(dev);
        env->cove_guest_access = false;
        if (val == 2) {
            env->cove_irq_iid[rid] = 0;
            env->cove_irq_file[rid] = 0;
            env->cove_rid_owner[rid] = 0;
            env->cove_claimed_rid[rid] = 0;
            memset(env->cove_bar_size[rid], 0, sizeof(env->cove_bar_size[rid]));
            memset(env->cove_bar_addr[rid], 0, sizeof(env->cove_bar_addr[rid]));
        }
    } else {
        return;
    }
    s->cove_control_status = 0;
    tlb_flush(env_cpu(env));
    queue_tb_flush(env_cpu(env));
}

static const MemoryRegionOps virt_cove_control_ops = {
    .read = virt_cove_control_read,
    .write = virt_cove_control_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};
