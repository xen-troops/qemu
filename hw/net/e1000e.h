#ifndef HW_E1000E_H
#define HW_E1000E_H

#define E1000E_MMIO_IDX     0
#define E1000E_FLASH_IDX    1
#define E1000E_IO_IDX       2
#define E1000E_MSIX_IDX     3

#define E1000E_MMIO_SIZE    (128 * KiB)
#define E1000E_FLASH_SIZE   (128 * KiB)
#define E1000E_IO_SIZE      (32)
#define E1000E_MSIX_SIZE    (16 * KiB)

#define TYPE_E1000E "e1000e"
#define E1000E(obj) OBJECT_CHECK(E1000EState, (obj), TYPE_E1000E)

typedef struct E1000EState {
    PCIDevice parent_obj;
    NICState *nic;
    NICConf conf;

    MemoryRegion mmio;
    MemoryRegion flash;
    MemoryRegion io;
    MemoryRegion msix;

    uint32_t ioaddr;

    uint16_t subsys_ven;
    uint16_t subsys;

    uint16_t subsys_ven_used;
    uint16_t subsys_used;

    bool disable_vnet;

    E1000ECore core;

} E1000EState;

typedef struct E1000EClassInitData {
    int msix_vec_num;
} E1000EClassInitData;

void e1000e_pci_realize(PCIDevice *pci_dev, Error **errp);
void e1000e_pci_uninit(PCIDevice *pci_dev);
void e1000e_class_init(ObjectClass *class, void *data);
void e1000e_qdev_reset(DeviceState *dev);

#endif /* HW_E1000E_H */
