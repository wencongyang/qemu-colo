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

#include "sysemu/sysemu.h"
#include "migration/migration-colo.h"
#include "trace.h"
#include "qemu/error-report.h"

enum {
    COLO_CHECPOINT_READY = 0x46,

    /*
    * Checkpoint synchronizing points.
    *
    *                  Primary                 Secondary
    *  NEW             @
    *                                          Suspend
    *  SUSPENDED                               @
    *                  Suspend&Save state
    *  SEND            @
    *                  Send state              Receive state
    *  RECEIVED                                @
    *                  Flush network           Load state
    *  LOADED                                  @
    *                  Resume                  Resume
    *
    *                  Start Comparing
    * NOTE:
    * 1) '@' who sends the message
    * 2) Every sync-point is synchronized by two sides with only
    *    one handshake(single direction) for low-latency.
    *    If more strict synchronization is required, a opposite direction
    *    sync-point should be added.
    * 3) Since sync-points are single direction, the remote side may
    *    go forward a lot when this side just receives the sync-point.
    */
    COLO_CHECKPOINT_NEW,
    COLO_CHECKPOINT_SUSPENDED,
    COLO_CHECKPOINT_SEND,
    COLO_CHECKPOINT_RECEIVED,
    COLO_CHECKPOINT_LOADED,
};

static QEMUBH *colo_bh;
static Coroutine *colo;
/* colo buffer */
#define COLO_BUFFER_BASE_SIZE (4 * 1024 * 1024)
QEMUSizedBuffer *colo_buffer;

bool colo_supported(void)
{
    return true;
}

bool migrate_in_colo_state(void)
{
    MigrationState *s = migrate_get_current();
    return (s->state == MIGRATION_STATUS_COLO);
}

bool loadvm_in_colo_state(void)
{
    return colo != NULL;
}

/* colo checkpoint control helper */
static int colo_ctl_put(QEMUFile *f, uint64_t request)
{
    int ret = 0;

    qemu_put_be64(f, request);
    qemu_fflush(f);

    ret = qemu_file_get_error(f);

    return ret;
}

static int colo_ctl_get_value(QEMUFile *f, uint64_t *value)
{
    int ret = 0;
    uint64_t temp;

    temp = qemu_get_be64(f);

    ret = qemu_file_get_error(f);
    if (ret < 0) {
        return -1;
    }

    *value = temp;
    return 0;
}

static int colo_ctl_get(QEMUFile *f, uint64_t require)
{
    int ret;
    uint64_t value;

    ret = colo_ctl_get_value(f, &value);
    if (ret < 0) {
        return ret;
    }

    if (value != require) {
        error_report("unexpected state! expected: %"PRIu64
                     ", received: %"PRIu64, require, value);
        exit(1);
    }

    return ret;
}

static int colo_do_checkpoint_transaction(MigrationState *s, QEMUFile *control)
{
    int ret;
    size_t size;
    QEMUFile *trans = NULL;

    ret = colo_ctl_put(s->file, COLO_CHECKPOINT_NEW);
    if (ret < 0) {
        goto out;
    }

    ret = colo_ctl_get(control, COLO_CHECKPOINT_SUSPENDED);
    if (ret < 0) {
        goto out;
    }
    /* Reset colo buffer and open it for write */
    qsb_set_length(colo_buffer, 0);
    trans = qemu_bufopen("w", colo_buffer);
    if (!trans) {
        error_report("Open colo buffer for write failed");
        goto out;
    }

    /* suspend and save vm state to colo buffer */
    qemu_mutex_lock_iothread();
    vm_stop_force_state(RUN_STATE_COLO);
    qemu_mutex_unlock_iothread();
    trace_colo_vm_state_change("run", "stop");

    /* Disable block migration */
    s->params.blk = 0;
    s->params.shared = 0;
    qemu_savevm_state_begin(trans, &s->params);
    qemu_mutex_lock_iothread();
    qemu_savevm_state_complete(trans);
    qemu_mutex_unlock_iothread();

    qemu_fflush(trans);

    ret = colo_ctl_put(s->file, COLO_CHECKPOINT_SEND);
    if (ret < 0) {
        goto out;
    }
    /* we send the total size of the vmstate first */
    size = qsb_get_length(colo_buffer);
    ret = colo_ctl_put(s->file, size);
    if (ret < 0) {
        goto out;
    }

    qsb_put_buffer(s->file, colo_buffer, size);
    qemu_fflush(s->file);
    ret = qemu_file_get_error(s->file);
    if (ret < 0) {
        goto out;
    }

    ret = colo_ctl_get(control, COLO_CHECKPOINT_RECEIVED);
    if (ret < 0) {
        goto out;
    }
    trace_colo_receive_message("COLO_CHECKPOINT_RECEIVED");

    ret = colo_ctl_get(control, COLO_CHECKPOINT_LOADED);
    if (ret < 0) {
        goto out;
    }
    trace_colo_receive_message("COLO_CHECKPOINT_LOADED");

    ret = 0;
    /* resume master */
    qemu_mutex_lock_iothread();
    vm_start();
    qemu_mutex_unlock_iothread();
    trace_colo_vm_state_change("stop", "run");

out:
    if (trans) {
        qemu_fclose(trans);
    }

    return ret;
}

static void *colo_thread(void *opaque)
{
    MigrationState *s = opaque;
    QEMUFile *colo_control = NULL;
    int ret;

    colo_control = qemu_fopen_socket(qemu_get_fd(s->file), "rb");
    if (!colo_control) {
        error_report("Open colo_control failed!");
        goto out;
    }

    /*
     * Wait for Secondary finish loading vm states and enter COLO
     * restore.
     */
    ret = colo_ctl_get(colo_control, COLO_CHECPOINT_READY);
    if (ret < 0) {
        goto out;
    }
    trace_colo_receive_message("COLO_CHECPOINT_READY");

    colo_buffer = qsb_create(NULL, COLO_BUFFER_BASE_SIZE);
    if (colo_buffer == NULL) {
        error_report("Failed to allocate colo buffer!");
        goto out;
    }

    qemu_mutex_lock_iothread();
    vm_start();
    qemu_mutex_unlock_iothread();
    trace_colo_vm_state_change("stop", "run");

    while (s->state == MIGRATION_STATUS_COLO) {
        /* start a colo checkpoint */
        if (colo_do_checkpoint_transaction(s, colo_control)) {
            goto out;
        }
    }

out:
    migrate_set_state(s, MIGRATION_STATUS_COLO, MIGRATION_STATUS_COMPLETED);

    qsb_free(colo_buffer);
    colo_buffer = NULL;

    if (colo_control) {
        qemu_fclose(colo_control);
    }

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

/*
 * return:
 * 0: start a checkpoint
 * -1: some error happened, exit colo restore
 */
static int colo_wait_handle_cmd(QEMUFile *f, int *checkpoint_request)
{
    int ret;
    uint64_t cmd;

    ret = colo_ctl_get_value(f, &cmd);
    if (ret < 0) {
        return -1;
    }

    switch (cmd) {
    case COLO_CHECKPOINT_NEW:
        *checkpoint_request = 1;
        return 0;
    default:
        return -1;
    }
}

void *colo_process_incoming_checkpoints(void *opaque)
{
    struct colo_incoming *colo_in = opaque;
    QEMUFile *f = colo_in->file;
    int fd = qemu_get_fd(f);
    QEMUFile *ctl = NULL, *fb = NULL;
    int ret;
    uint64_t total_size;

    colo = qemu_coroutine_self();
    assert(colo != NULL);

    ctl = qemu_fopen_socket(fd, "wb");
    if (!ctl) {
        error_report("Can't open incoming channel!");
        goto out;
    }

    if (create_and_init_ram_cache() < 0) {
        error_report("Failed to initialize ram cache");
        goto out;
    }

    colo_buffer = qsb_create(NULL, COLO_BUFFER_BASE_SIZE);
    if (colo_buffer == NULL) {
        error_report("Failed to allocate colo buffer!");
        goto out;
    }

    ret = colo_ctl_put(ctl, COLO_CHECPOINT_READY);
    if (ret < 0) {
        goto out;
    }

    qemu_mutex_lock_iothread();
    /* in COLO mode, slave is runing, so start the vm */
    vm_start();
    qemu_mutex_unlock_iothread();
    trace_colo_vm_state_change("stop", "run");

    while (true) {
        int request = 0;
        int ret = colo_wait_handle_cmd(f, &request);

        if (ret < 0) {
            break;
        } else {
            if (!request) {
                continue;
            }
        }

        /* suspend guest */
        qemu_mutex_lock_iothread();
        vm_stop_force_state(RUN_STATE_COLO);
        qemu_mutex_unlock_iothread();
        trace_colo_vm_state_change("run", "stop");

        ret = colo_ctl_put(ctl, COLO_CHECKPOINT_SUSPENDED);
        if (ret < 0) {
            goto out;
        }

        ret = colo_ctl_get(f, COLO_CHECKPOINT_SEND);
        if (ret < 0) {
            goto out;
        }
        trace_colo_receive_message("COLO_CHECKPOINT_SEND");

        /* read the VM state total size first */
        ret = colo_ctl_get_value(f, &total_size);
        if (ret < 0) {
            goto out;
        }

        /* read vm device state into colo buffer */
        ret = qsb_fill_buffer(colo_buffer, f, total_size);
        if (ret != total_size) {
            error_report("can't get all migration data");
            goto out;
        }

        ret = colo_ctl_put(ctl, COLO_CHECKPOINT_RECEIVED);
        if (ret < 0) {
            goto out;
        }
        trace_colo_receive_message("COLO_CHECKPOINT_RECEIVED");

        /* open colo buffer for read */
        fb = qemu_bufopen("r", colo_buffer);
        if (!fb) {
            error_report("can't open colo buffer for read");
            goto out;
        }

        qemu_mutex_lock_iothread();
        qemu_system_reset(VMRESET_SILENT);
        if (qemu_loadvm_state(fb) < 0) {
            error_report("COLO: loadvm failed");
            qemu_mutex_unlock_iothread();
            goto out;
        }
        qemu_mutex_unlock_iothread();

        /* TODO: flush vm state */

        ret = colo_ctl_put(ctl, COLO_CHECKPOINT_LOADED);
        if (ret < 0) {
            goto out;
        }

        /* resume guest */
        qemu_mutex_lock_iothread();
        vm_start();
        qemu_mutex_unlock_iothread();
        trace_colo_vm_state_change("stop", "start");

        qemu_fclose(fb);
        fb = NULL;
    }

out:
    colo = NULL;

    if (fb) {
        qemu_fclose(fb);
    }

    release_ram_cache();
    if (ctl) {
        qemu_fclose(ctl);
    }

    qsb_free(colo_buffer);

    loadvm_exit_colo();

    return NULL;
}
