/*
 * COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 * (a.k.a. Fault Tolerance or Continuous Replication)
 *
 * Copyright (c) 2015 HUAWEI TECHNOLOGIES CO.,LTD.
 * Copyright (c) 2015 FUJITSU LIMITED
 * Copyright (c) 2015 Intel Corporation
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later. See the COPYING file in the top-level directory.
 *
 */

#include <migration/migration-colo.h>

#define DEBUG_COLO_COMMON 0

#define DPRINTF(fmt, ...)                                  \
    do {                                                   \
        if (DEBUG_COLO_COMMON) {                           \
            fprintf(stderr, "COLO: " fmt, ## __VA_ARGS__); \
        }                                                  \
    } while (0)

static bool colo_requested;

/* save */
static void colo_info_save(QEMUFile *f, void *opaque)
{
    qemu_put_byte(f, migrate_enable_colo());
}

/* restore */
int get_colo_mode(void)
{
    if (migrate_in_colo_state()) {
        return COLO_PRIMARY_MODE;
    } else if (loadvm_in_colo_state()) {
        return COLO_SECONDARY_MODE;
    } else {
        return COLO_UNPROTECTED_MODE;
    }
}
static int colo_info_load(QEMUFile *f, void *opaque, int version_id)
{
    int value = qemu_get_byte(f);

    if (value && !colo_requested) {
        DPRINTF("COLO requested!\n");
    }
    colo_requested = value;

    return 0;
}

static SaveVMHandlers savevm_colo_info_handlers = {
    .save_state = colo_info_save,
    .load_state = colo_info_load,
};

void colo_info_mig_init(void)
{
    register_savevm_live(NULL, "colo", -1, 1,
                         &savevm_colo_info_handlers, NULL);
}

bool loadvm_enable_colo(void)
{
    return colo_requested;
}

void loadvm_exit_colo(void)
{
    colo_requested = false;
}
