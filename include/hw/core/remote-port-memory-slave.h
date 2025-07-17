// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * QEMU remote port memory slave. Read and write transactions
 * recieved from the remote port are translated into an address space.
 *
 * Copyright (c) 2013 Xilinx Inc
 * Written by Edgar E. Iglesias <edgar.iglesias@xilinx.com>
 * Written by Peter Crosthwaite <peter.crosthwaite@xilinx.com>
 *
 * This code is licensed under the GNU GPL.
 */
#ifndef REMOTE_PORT_MEMORY_SLAVE_H
#define REMOTE_PORT_MEMORY_SLAVE_H

#include "hw/core/remote-port-ats.h"

#define TYPE_REMOTE_PORT_MEMORY_SLAVE "remote-port-memory-slave"
#define REMOTE_PORT_MEMORY_SLAVE(obj) \
        OBJECT_CHECK(RemotePortMemorySlave, (obj), \
                     TYPE_REMOTE_PORT_MEMORY_SLAVE)

typedef struct RemotePortMemorySlave {
    /* private */
    SysBusDevice parent;
    /* public */
    struct RemotePort *rp;
    struct rp_peer_state *peer;
    MemoryRegion *mr;
    AddressSpace as;
    MemTxAttrs attr;
    RemotePortDynPkt rsp;
    RemotePortATSCache *ats_cache;
    Notifier machine_done;
    uint32_t rp_stream_id;
    RemotePortATS *rp_ats;
    uint32_t rp_ats_id;
    uint32_t iommu_id;
} RemotePortMemorySlave;
#endif
