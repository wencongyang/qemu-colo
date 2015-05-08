#include "qemu-common.h"
#include "block/block_int.h"
#include "block/blockjob.h"
#include "block/nbd.h"

typedef struct BDRVReplicationState {
    ReplicationMode mode;
    int replication_state;
    char *export_name;
    NBDExport *exp;
    BlockDriverState *active_disk;
    BlockDriverState *hidden_disk;
    int error;

    /*
     * After failover, secondary qemu should read/write this
     * bs directly.
     */
    BlockDriverState *bs;
} BDRVReplicationState;

enum {
    BLOCK_REPLICATION_NONE,     /* block replication is not started */
    BLOCK_REPLICATION_RUNNING,  /* block replication is running */
    BLOCK_REPLICATION_DONE,     /* block replication is done(failover) */
};

#define COMMIT_CLUSTER_BITS 16
#define COMMIT_CLUSTER_SIZE (1 << COMMIT_CLUSTER_BITS)
#define COMMIT_SECTORS_PER_CLUSTER (COMMIT_CLUSTER_SIZE / BDRV_SECTOR_SIZE)

static void replication_stop(BlockDriverState *bs, bool failover, Error **errp);

#define NBD_OPT_EXPORT          "export"
#define REPLICATION_MODE        "mode"
static QemuOptsList replication_runtime_opts = {
    .name = "replication",
    .head = QTAILQ_HEAD_INITIALIZER(replication_runtime_opts.head),
    .desc = {
        {
            .name = REPLICATION_MODE,
            .type = QEMU_OPT_STRING,
        },
        {
            .name = NBD_OPT_EXPORT,
            .type = QEMU_OPT_STRING,
            .help = "The NBD server name",
        },
        { /* end of list */ }
    },
};

static int replication_open(BlockDriverState *bs, QDict *options,
                            int flags, Error **errp)
{
    int ret;
    BDRVReplicationState *s = bs->opaque;;
    Error *local_err = NULL;
    QemuOpts *opts = NULL;
    const char *mode;

    ret = -EINVAL;
    opts = qemu_opts_create(&replication_runtime_opts, NULL, 0, &error_abort);
    qemu_opts_absorb_qdict(opts, options, &local_err);
    if (local_err) {
        goto fail;
    }

    mode = qemu_opt_get(opts, REPLICATION_MODE);
    if (!mode) {
        error_setg(&local_err, "Missing the option mode");
        goto fail;
    }

    if (!strcmp(mode, "primary")) {
        s->mode = REPLICATION_MODE_PRIMARY;
    } else if (!strcmp(mode, "secondary")) {
        s->mode = REPLICATION_MODE_SECONDARY;
    } else {
        error_setg(&local_err,
                   "The option mode's value should be primary or secondary");
        goto fail;
    }

    if (s->mode == REPLICATION_MODE_SECONDARY) {
        s->export_name = g_strdup(qemu_opt_get(opts, NBD_OPT_EXPORT));
        if (!s->export_name) {
            error_setg(&local_err, "Missing the option export");
            goto fail;
        }
    }

    return 0;

fail:
    qemu_opts_del(opts);
    /* propagate error */
    if (local_err) {
        error_propagate(errp, local_err);
    }
    return ret;
}

static void replication_close(BlockDriverState *bs)
{
    BDRVReplicationState *s = bs->opaque;

    if (s->replication_state == BLOCK_REPLICATION_RUNNING) {
        replication_stop(bs, false, NULL);
    }

    g_free(s->export_name);
}

static void replication_refresh_filename(BlockDriverState *bs)
{
}

static int64_t replication_getlength(BlockDriverState *bs)
{
    return bdrv_getlength(bs->file);
}

static int replication_get_io_status(BDRVReplicationState *s)
{
    switch (s->replication_state) {
    case BLOCK_REPLICATION_NONE:
        return -EIO;
    case BLOCK_REPLICATION_RUNNING:
        return 0;
    case BLOCK_REPLICATION_DONE:
        return s->mode == REPLICATION_MODE_PRIMARY ? -EIO : 1;
    default:
        abort();
    }
}

static int replication_return_value(BDRVReplicationState *s, int ret)
{
    if (s->mode == REPLICATION_MODE_SECONDARY) {
        return ret;
    }

    if (ret < 0) {
        s->error = ret;
        ret = 0;
    }

    return ret;
}

static coroutine_fn int replication_co_readv(BlockDriverState *bs,
                                             int64_t sector_num,
                                             int remaining_sectors,
                                             QEMUIOVector *qiov)
{
    BDRVReplicationState *s = bs->opaque;
    BlockDriverState *target;
    int ret;

    if (s->mode == REPLICATION_MODE_PRIMARY) {
        /* We only use it to forward primary write requests */
        return -EIO;
    }

    ret = replication_get_io_status(s);
    if (ret < 0) {
        return ret;
    }

    target = ret ? s->bs : bs->file;
    ret = bdrv_co_readv(target, sector_num, remaining_sectors, qiov);
    return replication_return_value(s, ret);
}

static coroutine_fn int replication_co_writev(BlockDriverState *bs,
                                              int64_t sector_num,
                                              int remaining_sectors,
                                              QEMUIOVector *qiov)
{
    BDRVReplicationState *s = bs->opaque;
    BlockDriverState *target;
    int ret;

    ret = replication_get_io_status(s);
    if (ret < 0) {
        return ret;
    }

    target = ret ? s->bs : bs->file;
    ret = bdrv_co_writev(target, sector_num, remaining_sectors, qiov);
    return replication_return_value(s, ret);
}

static coroutine_fn int replication_co_write_zeroes(BlockDriverState *bs,
                                                    int64_t sector_num,
                                                    int nb_sectors,
                                                    BdrvRequestFlags flags)
{
    BDRVReplicationState *s = bs->opaque;
    BlockDriverState *target;
    int ret;

    ret = replication_get_io_status(s);
    if (ret < 0) {
        return ret;
    }

    target = ret ? s->bs : bs->file;
    ret = bdrv_co_write_zeroes(target, sector_num, nb_sectors, flags);
    return replication_return_value(s, ret);
}

static coroutine_fn int replication_co_discard(BlockDriverState *bs,
                                               int64_t sector_num,
                                               int nb_sectors)
{
    BDRVReplicationState *s = bs->opaque;
    BlockDriverState *target;
    int ret;

    ret = replication_get_io_status(s);
    if (ret < 0) {
        return ret;
    }

    target = ret ? s->bs : bs->file;
    ret = bdrv_co_discard(target, sector_num, nb_sectors);
    return replication_return_value(s, ret);
}

static int replication_write_compressed(BlockDriverState *bs,
                                        int64_t sector_num, const uint8_t *buf,
                                        int nb_sectors)
{
    BDRVReplicationState *s = bs->opaque;
    BlockDriverState *target;
    int ret;

    ret = replication_get_io_status(s);
    if (ret < 0) {
        return ret;
    }

    target = ret ? s->bs : bs->file;
    ret = bdrv_write_compressed(target, sector_num, buf, nb_sectors);
    return replication_return_value(s, ret);
}

static bool replication_recurse_is_first_non_filter(BlockDriverState *bs,
                                                    BlockDriverState *candidate)
{
    return bdrv_recurse_is_first_non_filter(bs->file, candidate);
}

static void secondary_do_checkpoint(BDRVReplicationState *s, Error **errp)
{
    Error *local_err = NULL;
    int ret;

    if (!s->bs->job) {
        error_setg(errp, "Backup job is cancelled unexpectedly");
        return;
    }

    block_job_do_checkpoint(s->bs->job, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    ret = s->active_disk->drv->bdrv_make_empty(s->active_disk);
    if (ret < 0) {
        error_setg(errp, "Cannot make active disk empty");
        return;
    }

    ret = s->hidden_disk->drv->bdrv_make_empty(s->hidden_disk);
    if (ret < 0) {
        error_setg(errp, "Cannot make hidden disk empty");
        return;
    }
}

static void replication_start(BlockDriverState *bs, ReplicationMode mode,
                              Error **errp)
{
    BDRVReplicationState *s = bs->opaque;
    int64_t active_length, hidden_length, nbd_length;
    Error *local_err = NULL;

    if (s->replication_state != BLOCK_REPLICATION_NONE) {
        error_setg(errp, "Block replication is running or done");
        return;
    }

    if (s->mode != mode) {
        error_setg(errp, "Invalid parameter '%s'", "mode");
        return;
    }

    switch (s->mode) {
    case REPLICATION_MODE_PRIMARY:
        bdrv_connect(bs->file, errp);
        break;
    case REPLICATION_MODE_SECONDARY:
        if (!bs->file->backing_reference) {
            error_setg(errp, "Active disk doesn't use backing_reference");
        }

        s->active_disk = bs->file;
        s->hidden_disk = s->active_disk->backing_hd;
        s->bs = s->hidden_disk->backing_hd;

        if (!s->bs->job ||
            s->bs->job->driver->job_type != BLOCK_JOB_TYPE_BACKUP) {
            error_setg(errp, "Backup job is cancelled unexpectedly");
            return;
        }

        /* verify the length */
        active_length = bdrv_getlength(s->active_disk);
        hidden_length = bdrv_getlength(s->hidden_disk);
        nbd_length = bdrv_getlength(s->bs);
        if (active_length < 0 || hidden_length < 0 || nbd_length < 0 ||
            active_length != hidden_length || hidden_length != nbd_length) {
            error_setg(errp, "active disk, hidden disk, nbd target's length"
                       " are not the same");
            return;
        }

        if (!s->active_disk->drv->bdrv_make_empty ||
            !s->hidden_disk->drv->bdrv_make_empty) {
            error_setg(errp,
                       "active disk or hidden disk doesn't support make_empty");
            return;
        }
        break;
    default:
        abort();
    }

    s->replication_state = BLOCK_REPLICATION_RUNNING;

    if (s->mode == REPLICATION_MODE_SECONDARY) {
        secondary_do_checkpoint(s, errp);

        /* start NBD server */
        s->exp = nbd_export_new(s->bs->blk, 0, -1, 0, NULL, &local_err);
        if (!s->exp) {
            s->replication_state = BLOCK_REPLICATION_NONE;
            error_propagate(errp, local_err);
            return;
        }
        nbd_export_set_name(s->exp, s->export_name);
    }
}

static void replication_do_checkpoint(BlockDriverState *bs, Error **errp)
{
    BDRVReplicationState *s = bs->opaque;

    if (s->replication_state != BLOCK_REPLICATION_RUNNING) {
        error_setg(errp, "Block replication is not running");
        return;
    }

    if (s->error) {
        error_setg(errp, "I/O error occurs");
        return;
    }

    if (s->mode == REPLICATION_MODE_SECONDARY) {
        secondary_do_checkpoint(s, errp);
    }
}

/*
 * TODO: Use blockjob?
 */
static void commit_data(BlockDriverState *from, BlockDriverState *to,
                        int commit_buffer_sectors, Error **errp)
{
    int64_t len, target_length;
    int64_t sector_num, end;
    void *buf = NULL;
    int n = 0, ret;

    len = bdrv_getlength(from);
    target_length = bdrv_getlength(to);
    if (len < 0 || target_length < 0) {
        /* should not happen */
        error_setg(errp, "An undefined error has occurred");
        return;
    }

    assert(len == target_length);
    end = len >> BDRV_SECTOR_BITS;
    buf = qemu_blockalign(from, commit_buffer_sectors << BDRV_SECTOR_BITS);

    for (sector_num = 0; sector_num < end; sector_num += n) {
        ret = bdrv_is_allocated(from, sector_num, commit_buffer_sectors, &n);
        if (ret < 0) {
            error_setg(errp, "An undefined error has occurred");
            return;
        }

        if (ret == 0) {
            continue;
        }

        ret = bdrv_read(from, sector_num, buf, n);
        if (ret) {
            error_setg(errp, "An IO error has occurred");
            return;
        }

        ret = bdrv_write(to, sector_num, buf, n);
        if (ret) {
            error_setg(errp, "An IO error has occurred");
            return;
        }
    }
}

static void replication_stop(BlockDriverState *bs, bool failover, Error **errp)
{
    BDRVReplicationState *s = bs->opaque;
    Error *local_err = NULL;

    if (s->replication_state != BLOCK_REPLICATION_RUNNING) {
        error_setg(errp, "Block replication is not running");
        return;
    }

    s->replication_state = BLOCK_REPLICATION_DONE;

    switch (s->mode) {
    case REPLICATION_MODE_PRIMARY:
        bdrv_disconnect(bs->file);
        break;
    case REPLICATION_MODE_SECONDARY:
        /* stop NBD server */
        nbd_export_close(s->exp);
        nbd_export_put(s->exp);

        if (!failover) {
            secondary_do_checkpoint(s, errp);
            return;
        }

        if (!s->bs->job ||
            s->bs->job->driver->job_type != BLOCK_JOB_TYPE_BACKUP) {
            error_setg(errp, "Backup job is cancelled unexpectedly");
            return;
        }

        block_job_cancel(s->bs->job);

        /* commit data from active disk to hidden disk*/
        commit_data(s->active_disk, s->hidden_disk,
                    COMMIT_SECTORS_PER_CLUSTER, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            return;
        }

        /* commit data from hidden disk to nbd target */
        commit_data(s->hidden_disk, s->bs,
                    COMMIT_SECTORS_PER_CLUSTER, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            return;
        }
        break;
    default:
        abort();
    }
}

BlockDriver bdrv_replication = {
    .format_name                = "replication",
    .protocol_name              = "replication",
    .instance_size              = sizeof(BDRVReplicationState),

    .bdrv_open                  = replication_open,
    .bdrv_close                 = replication_close,
    .bdrv_refresh_filename      = replication_refresh_filename,

    .bdrv_getlength             = replication_getlength,
    .bdrv_co_readv              = replication_co_readv,
    .bdrv_co_writev             = replication_co_writev,
    .bdrv_co_write_zeroes       = replication_co_write_zeroes,
    .bdrv_co_discard            = replication_co_discard,
    .bdrv_write_compressed      = replication_write_compressed,

    .is_filter                  = true,
    .bdrv_recurse_is_first_non_filter = replication_recurse_is_first_non_filter,

    .bdrv_start_replication     = replication_start,
    .bdrv_do_checkpoint         = replication_do_checkpoint,
    .bdrv_stop_replication      = replication_stop,

    .has_variable_length        = true,
};

static void bdrv_replication_init(void)
{
    bdrv_register(&bdrv_replication);
}

block_init(bdrv_replication_init);
