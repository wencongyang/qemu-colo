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
 */

#include "sysemu/sysemu.h"
#include "migration/migration-colo.h"
#include "qemu/error-report.h"

#define DEBUG_COLO 0

#define DPRINTF(fmt, ...)                                   \
    do {                                                    \
        if (DEBUG_COLO) {                                   \
            fprintf(stderr, "colo: " fmt , ## __VA_ARGS__); \
        }                                                   \
    } while (0)

static QEMUBH *colo_bh;
static Coroutine *colo;

bool colo_supported(void)
{
    return true;
}

bool migrate_in_colo_state(void)
{
    MigrationState *s = migrate_get_current();
    return (s->state == MIGRATION_STATUS_COLO);
}

static void *colo_thread(void *opaque)
{
    MigrationState *s = opaque;

    qemu_mutex_lock_iothread();
    vm_start();
    qemu_mutex_unlock_iothread();
    DPRINTF("vm resume to run\n");


    /*TODO: COLO checkpoint savevm loop*/

    migrate_set_state(s, MIGRATION_STATUS_COLO, MIGRATION_STATUS_COMPLETED);

    qemu_mutex_lock_iothread();
    qemu_bh_schedule(s->cleanup_bh);
    qemu_mutex_unlock_iothread();

    return NULL;
}

static void colo_start_checkpointer(void *opaque)
{
    MigrationState *s = opaque;

    if (colo_bh) {
        qemu_bh_delete(colo_bh);
        colo_bh = NULL;
    }

    qemu_mutex_unlock_iothread();
    qemu_thread_join(&s->thread);
    qemu_mutex_lock_iothread();

    migrate_set_state(s, MIGRATION_STATUS_ACTIVE, MIGRATION_STATUS_COLO);

    qemu_thread_create(&s->thread, "colo", colo_thread, s,
                       QEMU_THREAD_JOINABLE);
}

void colo_init_checkpointer(MigrationState *s)
{
    colo_bh = qemu_bh_new(colo_start_checkpointer, s);
    qemu_bh_schedule(colo_bh);
}

void *colo_process_incoming_checkpoints(void *opaque)
{
    colo = qemu_coroutine_self();
    assert(colo != NULL);

    /* TODO: COLO checkpoint restore loop */

    colo = NULL;
    loadvm_exit_colo();

    return NULL;
}
