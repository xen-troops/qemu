/*
 * igb.c
 *
 * Intel 82576 Gigabit Ethernet Adapter
 * (SR/IOV capable PCIe ethernet device) emulation
 *
 * Copyright (c) 2014 Knut Omang <knut.omang@oracle.com>
 * Copyright (c) 2020 EPAM Systems Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

/* NB! This implementation does not work (yet) as an ethernet device,
 * it mainly serves as an example to demonstrate the emulated SR/IOV device
 * building blocks!
 *
 * You should be able to load both the PF and VF driver (igb/igbvf) on Linux
 * if the igb driver is loaded with a num_vfs != 0 driver parameter.
 * but the PF fails to reset (probably because that where the obvious
 * similarities between e1000 and igb ends..
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "net/net.h"
#include "net/tap.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"

#include "e1000x_common.h"
#include "e1000e_core.h"

#include "e1000e.h"


#define TYPE_IGB "igb"
#define IGB(obj) \
    OBJECT_CHECK(IgbState, (obj), TYPE_IGB)
#define TYPE_IGBVF "igbvf"
#define IGBVF(obj) \
    OBJECT_CHECK(IgbState, (obj), TYPE_IGBVF)

/* Experimental igb device (with SR/IOV) IDs for PF and VF: */
#define E1000_DEV_ID_82576      0x10C9
#define IGB_82576_VF_DEV_ID     0x10CA

#define I210_I_PHY_ID           0x01410C00

#define IGB_TOTAL_VFS           8
#define IGB_VF_OFFSET           0x80
#define IGB_VF_STRIDE           2
#define IGB_CAP_SRIOV_OFFSET    0x160

/* This number is derived as:
 *   adapter->rss_queues = min_t(u32, max_rss_queues, num_online_cpus());
 *   numvecs = 1 + 2 * rss (for Tx) + 2 * rss (for Rx)
 *   Max supported CPUs ATM is 8
 */
#define IGB_MSIX_VEC_NUM        (9)

static void pci_igb_realize(PCIDevice *pci_dev, Error **errp)
{
    Error *local_err = NULL;

    e1000e_pci_realize(pci_dev, &local_err);
    pcie_ari_init(pci_dev, 0x150, 1);
    pcie_sriov_pf_init(pci_dev, IGB_CAP_SRIOV_OFFSET, TYPE_IGBVF,
                       IGB_82576_VF_DEV_ID, IGB_TOTAL_VFS, IGB_TOTAL_VFS,
                       IGB_VF_OFFSET, IGB_VF_STRIDE);

    /* These are used by virtual functions */
    pcie_sriov_pf_init_vf_bar(pci_dev, E1000E_MMIO_IDX,
                              PCI_BASE_ADDRESS_MEM_TYPE_64,
                              E1000E_MMIO_SIZE);
    pcie_sriov_pf_init_vf_bar(pci_dev, E1000E_MSIX_IDX,
                              PCI_BASE_ADDRESS_MEM_TYPE_64,
                              E1000E_MSIX_SIZE);
}

static void pci_igbvf_realize(PCIDevice *pci_dev, Error **errp)
{
    Error *local_err = NULL;

    e1000e_pci_realize(pci_dev, &local_err);
    pcie_ari_init(pci_dev, 0x150, 1);
}

static void pci_igb_uninit(PCIDevice *pci_dev)
{
    pcie_sriov_pf_exit(pci_dev);
    pcie_cap_exit(pci_dev);
    e1000e_pci_uninit(pci_dev);
}

static void igb_reset(DeviceState *dev)
{
    PCIDevice *pci_dev = PCI_DEVICE(dev);
    E1000EState *s = E1000E(pci_dev);
    E1000ECore *core = &s->core;

    pcie_sriov_pf_disable_vfs(pci_dev);
    e1000e_qdev_reset(dev);

    core->phy[0][PHY_ID1] = I210_I_PHY_ID >> 16;
    core->phy[0][PHY_ID2] = I210_I_PHY_ID & 0xffff;
    core->mac[SWSM] = 0;
}

static void pci_igbvf_uninit(PCIDevice *pci_dev)
{
    pcie_cap_exit(pci_dev);
    e1000e_pci_uninit(pci_dev);
}

static void igb_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *c = PCI_DEVICE_CLASS(class);

    e1000e_class_init(class, data);

    c->realize = pci_igb_realize;
    c->exit = pci_igb_uninit;
    c->device_id = E1000_DEV_ID_82576;

    dc->reset = igb_reset;
    dc->desc = "Intel 82576 GbE Controller";
    /* Child doesn't add any new property. */
    dc->props = NULL;
}

static void igbvf_class_init(ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *c = PCI_DEVICE_CLASS(class);

    e1000e_class_init(class, data);

    c->device_id = IGB_82576_VF_DEV_ID;
    c->romfile = NULL;
    c->realize = pci_igbvf_realize;
    c->exit = pci_igbvf_uninit;
    /* Child doesn't add any new property. */
    dc->props = NULL;
}

static const E1000EClassInitData igb_class_data = {
    .msix_vec_num = 9,
};

static const TypeInfo igb_info = {
    .name      = TYPE_IGB,
    .parent    = TYPE_E1000E,
    .class_init = igb_class_init,
    .class_data = (void *)&igb_class_data,
};

static const TypeInfo igbvf_info = {
    .name      = TYPE_IGBVF,
    .parent    = TYPE_E1000E,
    .class_init = igbvf_class_init,
    .class_data = (void *)&igb_class_data,
};

static void igb_register_types(void)
{
    type_register_static(&igb_info);
    type_register_static(&igbvf_info);
}

type_init(igb_register_types)
