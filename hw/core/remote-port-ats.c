// SPDX-License-Identifier: MIT
/*
 * QEMU remote port ATS
 *
 * Copyright (c) 2021 Xilinx Inc
 * Written by Francisco Iglesias <francisco.iglesias@xilinx.com>
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
#include "system/system.h"
#include "system/dma.h"
#include "qemu/log.h"
#include "qapi/qmp/qerror.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"
#include "trace.h"
#include "qemu/cutils.h"

#include "hw/core/remote-port-proto.h"
#include "hw/core/remote-port-ats.h"

#define RP_TRACE_LVL_NONE   (0u)
#define RR_TRACE_LVL_ERROR  (1u)
#define RP_TRACE_LVL_WARN   (2u)
#define RP_TRACE_LVL_INFO   (3u)

#define RP_TRACE_LEVEL (RP_TRACE_LVL_INFO)

#ifndef RP_TRACE_LEVEL
#define RP_TRACE_LEVEL (RP_TRACE_LVL_NONE)
#endif

#define RP_TRACE(level, ...) do { \
    if (RP_TRACE_LEVEL >= level) { \
        fprintf(stderr,  "[TRACE] %s(): ", __func__); \
        fprintf(stderr, ## __VA_ARGS__); \
    } \
} while (0);

typedef struct ATSIOMMUNotifier {
    IOMMUNotifier n;
    MemoryRegion *mr;
    RemotePortATS *rp_ats;
    int iommu_idx;
} ATSIOMMUNotifier;

IOMMUTLBEntry *rp_ats_cache_lookup_translation(RemotePortATSCache *cache,
                                               hwaddr translated_addr,
                                               hwaddr len)
{
    RemotePortATSCacheClass *c = REMOTE_PORT_ATS_CACHE_GET_CLASS(cache);

    return c->lookup_translation(cache, translated_addr, len);
}

static IOMMUTLBEntry *rp_ats_lookup_translation(RemotePortATSCache *cache,
                                                hwaddr translated_addr,
                                                hwaddr len)
{
    RemotePortATS *s = REMOTE_PORT_ATS(cache);

    for (int i = 0; i < s->cache->len; i++) {
        IOMMUTLBEntry *iotlb = g_array_index(s->cache, IOMMUTLBEntry *, i);
        hwaddr masked_start = (translated_addr & ~iotlb->addr_mask);
        hwaddr masked_end = ((translated_addr + len - 1) & ~iotlb->addr_mask);

        if (masked_start == iotlb->translated_addr &&
            masked_end == iotlb->translated_addr) {
            return iotlb;
        }
    }

    return NULL;
}

static void rp_ats_cache_remove(RemotePortATS *s, IOMMUTLBEntry *iotlb)
{
    for (int i = 0; i < s->cache->len; i++) {
        IOMMUTLBEntry *tmp = g_array_index(s->cache, IOMMUTLBEntry *, i);
        hwaddr masked_start = (tmp->iova & ~iotlb->addr_mask);
        hwaddr masked_end = ((tmp->iova | tmp->addr_mask) & ~iotlb->addr_mask);

        if (masked_start == iotlb->iova || masked_end == iotlb->iova) {
            g_array_remove_index_fast(s->cache, i);
        }
    }
}

static void rp_ats_invalidate(RemotePortATS *s, IOMMUTLBEntry *iotlb)
{
    size_t pktlen = sizeof(struct rp_pkt_ats);
    struct rp_pkt_ats pkt;
    RemotePortRespSlot *rsp_slot;
    RemotePortDynPkt *rsp;
    size_t enclen;
    int64_t clk;
    uint32_t id;
    hwaddr len = iotlb->addr_mask + 1;

    id = rp_new_id(s->rp);
    clk = rp_normalized_vmclk(s->rp);

    enclen = rp_encode_ats_inv(id, s->rp_dev,
                             &pkt,
                             clk,
                             0,
                             iotlb->iova,
                             len,
                             0,
                             0);
    assert(enclen == pktlen);

    rp_rsp_mutex_lock(s->rp);
    rp_write(s->rp, (void *) &pkt, enclen);

    rsp_slot = rp_dev_wait_resp(s->rp, s->rp_dev, id);
    rsp = &rsp_slot->rsp;

    /* We dont support out of order answers yet.  */
    assert(rsp->pkt->hdr.id == id);

    rp_resp_slot_done(s->rp, rsp_slot);
    rp_rsp_mutex_unlock(s->rp);
}

static void rp_ats_cache_insert(RemotePortATS *s,
                                hwaddr iova,
                                hwaddr translated_addr,
                                hwaddr mask,
                                AddressSpace *target_as)
{
    IOMMUTLBEntry *iotlb;

    /*
     * Invalidate all current translations that collide with the new one and
     * does not have the same target_as. This means that translated_addresses
     * towards the same addresses but in different target address spaces are
     * not allowed.
     */
    for (int i = 0; i < s->cache->len; i++) {
        iotlb = g_array_index(s->cache, IOMMUTLBEntry *, i);
        hwaddr masked_start = (translated_addr & ~iotlb->addr_mask);
        hwaddr masked_end = ((translated_addr | mask) & ~iotlb->addr_mask);
        bool spans_region = masked_start < iotlb->translated_addr &&
                             masked_end > iotlb->translated_addr;

        if (masked_start == iotlb->translated_addr ||
            masked_end == iotlb->translated_addr || spans_region) {
            hwaddr masked_iova_start;
            hwaddr masked_iova_end;

            /*
             * Invalidated & remove the mapping if the address range hit in the
             * cache but the target_as is different.
             */
            if (iotlb->target_as != target_as) {
                rp_ats_invalidate(s, iotlb);
                g_array_remove_index_fast(s->cache, i);
                continue;
            }

            /*
             * Remove duplicates with a smaller range length since the new
             * mapping will span over it.
             */
            masked_iova_start = (iova & ~iotlb->addr_mask);
            masked_iova_end = ((iova | mask) & ~iotlb->addr_mask);
            spans_region = masked_iova_start < iotlb->iova &&
                            masked_iova_end > iotlb->iova;

            if (masked_iova_start == iotlb->iova ||
                masked_iova_end == iotlb->iova || spans_region) {

                if ((iotlb->addr_mask + 1) < (mask + 1)) {
                    g_array_remove_index_fast(s->cache, i);
                } else {
                    /*
                     * The new mapping is smaller or equal in size and is thus
                     * already cached.
                     */
                    return;
                }
            }
        }
    }

    iotlb = g_new0(IOMMUTLBEntry, 1);
    iotlb->iova = iova;
    iotlb->translated_addr = translated_addr;
    iotlb->addr_mask = mask;
    iotlb->target_as = target_as;
    g_array_append_val(s->cache, iotlb);

    char *target_as_name = strdup(iotlb->target_as->name); 
    RP_TRACE(RP_TRACE_LVL_INFO, "IOMMUTLBEntry entry added to cache\r\n");
    RP_TRACE(RP_TRACE_LVL_INFO, "iova: 0x%" PRIx64 " translated_addr: 0x%" \
       PRIx64 " addr_mask: 0x%" PRIx64 " target_as: %s perm: 0x%" PRIx32 "\r\n",
        iotlb->iova,
        iotlb->translated_addr,
        iotlb->addr_mask,
        target_as_name,
        iotlb->perm);
    free(target_as_name);
}

static void rp_ats_iommu_unmap_notify(IOMMUNotifier *n, IOMMUTLBEntry *iotlb)
{
    ATSIOMMUNotifier *notifier = container_of(n, ATSIOMMUNotifier, n);
    RemotePortATS *s = notifier->rp_ats;

    rp_ats_invalidate(s, iotlb);
    rp_ats_cache_remove(s, iotlb);
}

static bool ats_translate_address(RemotePortATS *s, struct rp_pkt *pkt,
                                  hwaddr *phys_addr, hwaddr *phys_len)
{
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    IOMMUMemoryRegion *iommu_mr;
    AddressSpace *target_as;
    MemoryRegion *mr;
    int prot = 0;

    RCU_READ_LOCK_GUARD();

    mr = ats_do_translate(&s->as, pkt->ats.addr, phys_addr, phys_len,
                          &target_as, &prot, attrs);
    if (!mr) {
        RP_TRACE(RP_TRACE_LVL_INFO, "Remote Port ATS translation failed\r\n");
        return false;
    }

    RP_TRACE(RP_TRACE_LVL_INFO,
        "Remote Port ATS translation succeeded - aka. ats_do_translate()\r\n");
    RP_TRACE(RP_TRACE_LVL_INFO,
        "phys_addr: 0x%" PRIx64 " phys_len: 0x%" PRIx64 "\r\n",
        *phys_addr, *phys_len);

    iommu_mr = memory_region_get_iommu(mr);
    if (iommu_mr) {
        RP_TRACE(RP_TRACE_LVL_INFO,
        "Memory region obtained from ats_do_translate() is associated with IOMMU\r\n");
        int iommu_idx = memory_region_iommu_attrs_to_index(iommu_mr, attrs);
        ATSIOMMUNotifier *notifier;
        int i;

        for (i = 0; i < s->iommu_notifiers->len; i++) {
            notifier = g_array_index(s->iommu_notifiers, ATSIOMMUNotifier *, i);
            if (notifier->mr == mr && notifier->iommu_idx == iommu_idx) {
                break;
            }
        }

        /* Register a notifier if not found.  */
        if (i == s->iommu_notifiers->len) {
            Error *err = NULL;
            bool ret;

            s->iommu_notifiers = g_array_set_size(s->iommu_notifiers, i + 1);
            notifier = g_new0(ATSIOMMUNotifier, 1);
            g_array_index(s->iommu_notifiers, ATSIOMMUNotifier *, i) = notifier;

            notifier->mr = mr;
            notifier->iommu_idx = iommu_idx;
            notifier->rp_ats = s;

            iommu_notifier_init(&notifier->n,
                                rp_ats_iommu_unmap_notify,
                                IOMMU_NOTIFIER_UNMAP,
                                0,
                                HWADDR_MAX,
                                iommu_idx);

            ret = memory_region_register_iommu_notifier(mr, &notifier->n, &err);
            if (ret) {
                error_report_err(err);
                exit(1);
            }
        }
    }

    if (!(prot & IOMMU_RO)) {
        pkt->ats.attributes &= ~(RP_ATS_ATTR_exec | RP_ATS_ATTR_read);
    }
    if (!(prot & IOMMU_WO)) {
        pkt->ats.attributes &= ~(RP_ATS_ATTR_write);
    }

    rp_ats_cache_insert(s, pkt->ats.addr, *phys_addr, *phys_len - 1, target_as);

    return true;
}

static void rp_ats_req(RemotePortDevice *dev, struct rp_pkt *pkt)
{
    RemotePortATS *s = REMOTE_PORT_ATS(dev);
    size_t pktlen = sizeof(struct rp_pkt_ats);
    hwaddr phys_addr = 0;
    hwaddr phys_len = (hwaddr)(-1);
    uint64_t result;
    size_t enclen;
    int64_t delay;
    int64_t clk;

    assert(!(pkt->hdr.flags & RP_PKT_FLAGS_response));

    RP_TRACE(RP_TRACE_LVL_INFO, "Remote Port ATS request is received\r\n")
    RP_TRACE(RP_TRACE_LVL_INFO, "cmd 0x%" PRIx32 " len: 0x%" PRIx32 " id: 0x%" \
        PRIx32 " flags: 0x%" PRIx32 " dev: 0x%" PRIx32 "\r\n",
        pkt->hdr.cmd, pkt->hdr.len, pkt->hdr.id, pkt->hdr.flags, pkt->hdr.dev);
    RP_TRACE(RP_TRACE_LVL_INFO, "timestamp 0x%" PRIx64 " attributes: 0x%" PRIx64 
        " addr: 0x%" PRIx64 " len: 0x%" PRIx64 " result: 0x%" PRIx32 "\r\n",
        pkt->ats.timestamp, pkt->ats.attributes, pkt->ats.addr, pkt->ats.len,
        pkt->ats.result);

    rp_dpkt_alloc(&s->rsp, pktlen);

    result = ats_translate_address(s, pkt, &phys_addr, &phys_len) ?
        RP_ATS_RESULT_ok : RP_ATS_RESULT_error;

    RP_TRACE(RP_TRACE_LVL_INFO, "Remote Port ATS translation is %s\r\n",
            ((result == RP_ATS_RESULT_ok) ? "successful" : "unsuccessful"));

    /*
     * delay here could be set to the annotated cost of doing issuing
     * these accesses. QEMU doesn't support this kind of annotations
     * at the moment. So we just clear the delay.
     */
    delay = 0;
    clk = pkt->ats.timestamp + delay;

    enclen = rp_encode_ats_req(pkt->hdr.id, pkt->hdr.dev,
                             &s->rsp.pkt->ats,
                             clk,
                             pkt->ats.attributes,
                             phys_addr,
                             phys_len,
                             result,
                             pkt->hdr.flags | RP_PKT_FLAGS_response);
    assert(enclen == pktlen);

    RP_TRACE(RP_TRACE_LVL_INFO, "Remote Port ATS response is generated\r\n")
    RP_TRACE(RP_TRACE_LVL_INFO, "cmd 0x%" PRIx32 " len: 0x%" PRIx32 " id: 0x%" \
        PRIx32 " flags: 0x%" PRIx32 " dev: 0x%" PRIx32 "\r\n",
        be32toh(s->rsp.pkt->hdr.cmd), 
        be32toh(s->rsp.pkt->hdr.len), 
        be32toh(s->rsp.pkt->hdr.id), 
        be32toh(s->rsp.pkt->hdr.flags), 
        be32toh(s->rsp.pkt->hdr.dev));
    RP_TRACE(RP_TRACE_LVL_INFO, "timestamp 0x%" PRIx64 " attributes: 0x%" \
    PRIx64 " addr: 0x%" PRIx64 " len: 0x%" PRIx64 " result: 0x%" PRIx32 "\r\n",
        be64toh(s->rsp.pkt->ats.timestamp),
        be64toh(s->rsp.pkt->ats.attributes),
        be64toh(s->rsp.pkt->ats.addr),
        be64toh(s->rsp.pkt->ats.len),
        be32toh(s->rsp.pkt->ats.result));

    rp_write(s->rp, (void *)s->rsp.pkt, enclen);
}

static AddressSpace *bus_iommu_address_space(BusState *iommu_bus,
                                             uint32_t iommu_id, uint16_t devid)
{
    if (iommu_bus && iommu_bus->iommu[iommu_id].iommu_ops) {
        return iommu_bus->iommu[iommu_id].iommu_ops->get_address_space(
            iommu_bus, iommu_bus->iommu[iommu_id].iommu_opaque, devid);
    }
    return &address_space_memory;
}

static void rp_ats_init_done(Notifier *notifier, void *data)
{
    RemotePortATS *s = container_of(notifier, RemotePortATS, machine_done);
    AddressSpace *as;

    if (s->rp_stream_id) {
        as = bus_iommu_address_space(sysbus_get_default(),
                                     s->iommu_id, s->rp_stream_id);
        address_space_init(&s->as, as->root, "dma");
    } else {
        address_space_init(&s->as, s->mr ? s->mr : get_system_memory(), "dma");
    }
}

static void rp_ats_realize(DeviceState *dev, Error **errp)
{
    RemotePortATS *s = REMOTE_PORT_ATS(dev);

    s->peer = rp_get_peer(s->rp);

    s->iommu_notifiers = g_array_new(false, true, sizeof(ATSIOMMUNotifier *));
    s->cache = g_array_new(false, true, sizeof(IOMMUTLBEntry *));

    s->machine_done.notify = rp_ats_init_done;
    qemu_add_machine_init_done_notifier(&s->machine_done);
}

static void rp_prop_allow_set_link(const Object *obj, const char *name,
                                   Object *val, Error **errp)
{
}

static void rp_ats_init(Object *obj)
{
    RemotePortATS *s = REMOTE_PORT_ATS(obj);

    object_property_add_link(obj, "rp-adaptor0", "remote-port",
                             (Object **)&s->rp,
                             rp_prop_allow_set_link,
                             OBJ_PROP_LINK_STRONG);
    object_property_add_link(obj, "mr", TYPE_MEMORY_REGION,
                             (Object **)&s->mr,
                             qdev_prop_allow_set_link_before_realize,
                             OBJ_PROP_LINK_STRONG);
}

static void rp_ats_unrealize(DeviceState *dev)
{
    RemotePortATS *s = REMOTE_PORT_ATS(dev);
    ATSIOMMUNotifier *notifier;
    int i;

    for (i = 0; i < s->iommu_notifiers->len; i++) {
        notifier = g_array_index(s->iommu_notifiers, ATSIOMMUNotifier *, i);
        memory_region_unregister_iommu_notifier(notifier->mr, &notifier->n);
        g_free(notifier);
    }
    g_array_free(s->iommu_notifiers, true);

    address_space_destroy(&s->as);

    for (i = 0; i < s->cache->len; i++) {
        IOMMUTLBEntry *tmp = g_array_index(s->cache, IOMMUTLBEntry *, i);
        g_free(tmp);
    }
    g_array_free(s->cache, true);
}

static Property rp_properties[] = {
    DEFINE_PROP_UINT32("iommu-id", RemotePortATS, iommu_id, 0),
    DEFINE_PROP_UINT32("rp-chan0", RemotePortATS, rp_dev, 0),
    DEFINE_PROP_UINT32("rp-stream-id", RemotePortATS, rp_stream_id, 0),
};

static void rp_ats_class_init(ObjectClass *oc, const void *data)
{
    RemotePortDeviceClass *rpdc = REMOTE_PORT_DEVICE_CLASS(oc);
    RemotePortATSCacheClass *atscc = REMOTE_PORT_ATS_CACHE_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_props_n(dc, rp_properties, ARRAY_SIZE(rp_properties));

    rpdc->ops[RP_CMD_ats_req] = rp_ats_req;
    dc->realize = rp_ats_realize;
    dc->unrealize = rp_ats_unrealize;
    atscc->lookup_translation = rp_ats_lookup_translation;
}

static const TypeInfo rp_ats_info = {
    .name          = TYPE_REMOTE_PORT_ATS,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(RemotePortATS),
    .instance_init = rp_ats_init,
    .class_init    = rp_ats_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { TYPE_REMOTE_PORT_DEVICE },
        { TYPE_REMOTE_PORT_ATS_CACHE },
        { },
    },
};

static const TypeInfo rp_ats_cache_info = {
    .name          = TYPE_REMOTE_PORT_ATS_CACHE,
    .parent        = TYPE_INTERFACE,
    .class_size = sizeof(RemotePortATSCacheClass),
};

static void rp_register_types(void)
{
    type_register_static(&rp_ats_cache_info);
    type_register_static(&rp_ats_info);
}

type_init(rp_register_types)
