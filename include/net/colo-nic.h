/*
 * COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 * (a.k.a. Fault Tolerance or Continuous Replication)
 *
 * Copyright (c) 2015 HUAWEI TECHNOLOGIES CO.,LTD.
 * Copyright (c) 2015 FUJITSU LIMITED
 * Copyright (c) 2015 Intel Corporation
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 *
 */

#ifndef COLO_NIC_H
#define COLO_NIC_H

int colo_proxy_init(int side);
void colo_proxy_destroy(int side);
void colo_add_nic_devices(NetClientState *nc);
void colo_remove_nic_devices(NetClientState *nc);

int colo_proxy_compare(void);

#endif
