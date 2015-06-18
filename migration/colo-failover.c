/*
 * COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 * (a.k.a. Fault Tolerance or Continuous Replication)
 *
 * Copyright (c) 2015 HUAWEI TECHNOLOGIES CO., LTD.
 * Copyright (c) 2015 FUJITSU LIMITED
 * Copyright (c) 2015 Intel Corporation
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#include "migration/migration-colo.h"
#include "migration/migration-failover.h"
#include "qmp-commands.h"
#include "qapi/qmp/qerror.h"

static bool failover_request;

static QEMUBH *failover_bh;

static void colo_failover_bh(void *opaque)
{
    qemu_bh_delete(failover_bh);
    failover_bh = NULL;
    colo_do_failover(NULL);
}

void failover_request_set(void)
{
    failover_request = true;
    failover_bh = qemu_bh_new(colo_failover_bh, NULL);
    qemu_bh_schedule(failover_bh);
}

void failover_request_clear(void)
{
    failover_request = false;
}

bool failover_request_is_set(void)
{
    return failover_request;
}

void qmp_colo_lost_heartbeat(Error **errp)
{
    if (get_colo_mode() == COLO_MODE_UNKNOWN) {
        error_setg(errp, QERR_FEATURE_DISABLED, "colo");
        return;
    }
    failover_request_set();
}
