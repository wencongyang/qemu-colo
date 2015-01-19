/*
 * Block driver for block replication
 *
 * Copyright Fujitsu, Corp. 2015
 * Copyright (c) 2015 Intel Corporation
 * Copyright (c) 2015 HUAWEI TECHNOLOGIES CO.,LTD.
 *
 * Authors:
 *     Wen Congyang <wency@cn.fujitsu.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "block/block_int.h"
#include "sysemu/block-backend.h"
#include "block/blkcolo.h"
#include "block/nbd.h"

#define COLO_OPT_EXPORT         "export"

#define COLO_CLUSTER_BITS 16
#define COLO_CLUSTER_SIZE (1 << COLO_CLUSTER_BITS)
#define COLO_SECTORS_PER_CLUSTER (COLO_CLUSTER_SIZE / BDRV_SECTOR_SIZE)

typedef struct BDRVBlkcoloState BDRVBlkcoloState;

struct BDRVBlkcoloState {
    BlockDriverState *bs;
    char *export_name;
    int mode;
    disk_buffer disk_buffer;
    NotifierWithReturn before_write;
    NBDExport *exp;
    CowJob cow_job;
    bool error;
};

static void colo_svm_init(BDRVBlkcoloState *s);
static void colo_svm_fini(BDRVBlkcoloState *s);

static int switch_mode(BDRVBlkcoloState *s, int new_mode)
{
    if (s->mode == new_mode) {
        return 0;
    }

    if (s->mode == COLO_SECONDARY_MODE) {
        colo_svm_fini(s);
    }

    s->mode = new_mode;
    if (s->mode == COLO_SECONDARY_MODE) {
        colo_svm_init(s);
    }

    return 0;
}

/*
 * Secondary mode functions
 *
 * All write requests are forwarded to secondary QEMU from primary QEMU.
 * The secondary QEMU should do the following things:
 * 1. Use NBD server to receive and handle the forwarded write requests
 * 2. Buffer the secondary write requests
 */

static int coroutine_fn
colo_svm_co_writev(BlockDriverState *bs, int64_t sector_num,
                   int nb_sectors, QEMUIOVector *qiov)
{
    BDRVBlkcoloState *s = bs->opaque;

    /*
     * Write the request to the disk buffer. How to limit the
     * write speed?
     */
    qiov_write_to_buffer(&s->disk_buffer, qiov, sector_num, nb_sectors, true);

    return 0;
}

static int coroutine_fn
colo_svm_co_readv(BlockDriverState *bs, int64_t sector_num,
                  int nb_sectors, QEMUIOVector *qiov)
{
    BDRVBlkcoloState *s = bs->opaque;
    int ret;

    /*
     * Read the sector content from secondary disk first. If the sector
     * content is buffered, use the buffered content.
     */
    ret = bdrv_co_readv(bs->backing_hd, sector_num, nb_sectors, qiov);
    if (ret) {
        return ret;
    }

    /* Read from the buffer */
    qiov_read_from_buffer(&s->disk_buffer, qiov, sector_num, nb_sectors);
    return 0;
}

static int coroutine_fn
colo_do_cow(BlockDriverState *bs, int64_t sector_num, int nb_sectors)
{
    BDRVBlkcoloState *s = bs->origin_file->opaque;
    CowRequest cow_request;
    struct iovec iov;
    QEMUIOVector bounce_qiov;
    void *bounce_buffer = NULL;
    int ret = 0;
    int64_t start, end;
    int n;

    start = sector_num / COLO_SECTORS_PER_CLUSTER;
    end = DIV_ROUND_UP(sector_num + nb_sectors, COLO_SECTORS_PER_CLUSTER);

    wait_for_overlapping_requests(&s->cow_job, start, end);
    cow_request_begin(&cow_request, &s->cow_job, start, end);

    nb_sectors = COLO_SECTORS_PER_CLUSTER;
    for (; start < end; start++) {
        sector_num = start * COLO_SECTORS_PER_CLUSTER;
        if (!buffer_has_empty_range(&s->disk_buffer, sector_num, nb_sectors)) {
            continue;
        }

        /* TODO */
        n = COLO_SECTORS_PER_CLUSTER;

        if (!bounce_buffer) {
            bounce_buffer = qemu_blockalign(bs, COLO_CLUSTER_SIZE);
        }
        iov.iov_base = bounce_buffer;
        iov.iov_len = n * BDRV_SECTOR_SIZE;
        qemu_iovec_init_external(&bounce_qiov, &iov, 1);

        ret = bdrv_co_readv(bs, sector_num, n, &bounce_qiov);
        if (ret < 0) {
            goto out;
        }

        qiov_write_to_buffer(&s->disk_buffer, &bounce_qiov,
                             sector_num, n, false);
    }

out:
    cow_request_end(&cow_request);
    return ret;
}

static int coroutine_fn
colo_before_write_notify(NotifierWithReturn *notifier, void *opaque)
{
    BdrvTrackedRequest *req = opaque;
    BlockDriverState *bs = req->bs;
    BDRVBlkcoloState *s = bs->origin_file->opaque;
    int64_t sector_num = req->offset >> BDRV_SECTOR_BITS;
    int nb_sectors = req->bytes >> BDRV_SECTOR_BITS;
    int ret;

    assert((req->offset & (BDRV_SECTOR_SIZE - 1)) == 0);
    assert((req->bytes & (BDRV_SECTOR_SIZE - 1)) == 0);

    ret = colo_do_cow(bs, sector_num, nb_sectors);
    if (ret) {
        s->error = true;
    }

    return ret;
}

/*
 * It should be called in the migration/checkpoint thread, and the caller
 * should be hold io thread lock
 */
static int svm_do_checkpoint(BDRVBlkcoloState *s)
{
    if (s->error) {
        /* TODO: we should report the error more earlier */
        return -1;
    }

    /* clear disk buffer */
    clear_all_buffered_data(&s->disk_buffer);
    return 0;
}

/* It should be called in the migration/checkpoint thread */
static void svm_stop_replication(BDRVBlkcoloState *s)
{
    /* switch to unprotected mode */
    switch_mode(s, COLO_UNPROTECTED_MODE);
}

static void colo_svm_init(BDRVBlkcoloState *s)
{
    BlockBackend *blk = s->bs->backing_hd->blk;

    /* Init Disk Buffer */
    init_disk_buffer(&s->disk_buffer);

    s->before_write.notify = colo_before_write_notify;
    bdrv_add_before_write_notifier(s->bs->backing_hd, &s->before_write);

    /* start NBD server */
    s->exp = nbd_export_new(blk, 0, -1, 0, NULL);
    nbd_export_set_name(s->exp, s->export_name);

    s->error = false;
    QLIST_INIT(&s->cow_job.inflight_reqs);
}

static void colo_svm_fini(BDRVBlkcoloState *s)
{
    /* stop NBD server */
    nbd_export_close(s->exp);
    nbd_export_put(s->exp);

    /* notifier_with_return_remove */
    notifier_with_return_remove(&s->before_write);

    /* TODO: All pvm write requests have been done? */

    /* flush all buffered data to secondary disk */
    flush_buffered_data_to_disk(&s->disk_buffer, s->bs->backing_hd);
}

/* block driver interfaces */
static QemuOptsList colo_runtime_opts = {
    .name = "colo",
    .head = QTAILQ_HEAD_INITIALIZER(colo_runtime_opts.head),
    .desc = {
        {
            .name = COLO_OPT_EXPORT,
            .type = QEMU_OPT_STRING,
            .help = "The NBD server name",
        },
        { /* end of list */ }
    },
};

/*
 * usage: -drive if=xxx,driver=colo,export=xxx,\
 *        backing.file.filename=1.raw,\
 *        backing.driver=raw
 */
static int blkcolo_open(BlockDriverState *bs, QDict *options, int flags,
                        Error **errp)
{
    BDRVBlkcoloState *s = bs->opaque;
    Error *local_err = NULL;
    QemuOpts *opts = NULL;
    int ret = 0;

    s->bs = bs;

    opts = qemu_opts_create(&colo_runtime_opts, NULL, 0, &error_abort);
    qemu_opts_absorb_qdict(opts, options, &local_err);
    if (local_err) {
        ret = -EINVAL;
        goto exit;
    }

    s->export_name = g_strdup(qemu_opt_get(opts, COLO_OPT_EXPORT));
    if (!s->export_name) {
        error_setg(&local_err, "Missing the option export");
        ret = -EINVAL;
        goto exit;
    }

exit:
    qemu_opts_del(opts);
    /* propagate error */
    if (local_err) {
        error_propagate(errp, local_err);
    }
    return ret;
}

static void blkcolo_close(BlockDriverState *bs)
{
    BDRVBlkcoloState *s = bs->opaque;

    if (s->mode == COLO_SECONDARY_MODE) {
        switch_mode(s, COLO_UNPROTECTED_MODE);
    }

    g_free(s->export_name);
}

static int64_t blkcolo_getlength(BlockDriverState *bs)
{
    if (!bs->backing_hd) {
        return 0;
    } else {
        return bdrv_getlength(bs->backing_hd);
    }
}

static int blkcolo_co_readv(BlockDriverState *bs, int64_t sector_num,
                            int nb_sectors, QEMUIOVector *qiov)
{
    BDRVBlkcoloState *s = bs->opaque;

    if (s->mode == COLO_SECONDARY_MODE) {
        return colo_svm_co_readv(bs, sector_num, nb_sectors, qiov);
    }

    assert(s->mode == COLO_UNPROTECTED_MODE);

    if (!bs->backing_hd) {
        return -EIO;
    } else {
        return bdrv_co_readv(bs->backing_hd, sector_num, nb_sectors, qiov);
    }
}

static int blkcolo_co_writev(BlockDriverState *bs, int64_t sector_num,
                             int nb_sectors, QEMUIOVector *qiov)
{
    BDRVBlkcoloState *s = bs->opaque;

    if (s->mode == COLO_SECONDARY_MODE) {
        return colo_svm_co_writev(bs, sector_num, nb_sectors, qiov);
    }

    assert(s->mode == COLO_UNPROTECTED_MODE);

    if (!bs->backing_hd) {
        return -EIO;
    } else {
        return bdrv_co_writev(bs->backing_hd, sector_num, nb_sectors, qiov);
    }
}

static int blkcolo_start_replication(BlockDriverState *bs, int mode)
{
    BDRVBlkcoloState *s = bs->opaque;

    if (mode != COLO_SECONDARY_MODE ||
        s->mode != COLO_UNPROTECTED_MODE ||
        !bs->backing_hd) {
        return -1;
    }

    if (!blk_is_inserted(bs->backing_hd->blk)) {
        return -1;
    }

    if (blk_is_read_only(bs->backing_hd->blk)) {
        return -1;
    }

    return switch_mode(s, mode);
}

static int blkcolo_do_checkpoint(BlockDriverState *bs)
{
    BDRVBlkcoloState *s = bs->opaque;

    if (s->mode != COLO_SECONDARY_MODE) {
        return -1;
    }

    return svm_do_checkpoint(s);
}

static int blkcolo_stop_replication(BlockDriverState *bs)
{
    BDRVBlkcoloState *s = bs->opaque;

    if (s->mode != COLO_SECONDARY_MODE) {
        return -1;
    }

    svm_stop_replication(s);
    return 0;
}

static BlockDriver bdrv_blkcolo = {
    .format_name                = "blkcolo",
    .protocol_name              = "blkcolo",
    .instance_size              = sizeof(BDRVBlkcoloState),

    .bdrv_file_open             = blkcolo_open,
    .bdrv_close                 = blkcolo_close,
    .bdrv_getlength             = blkcolo_getlength,

    .bdrv_co_readv              = blkcolo_co_readv,
    .bdrv_co_writev             = blkcolo_co_writev,

    .bdrv_start_replication     = blkcolo_start_replication,
    .bdrv_do_checkpoint         = blkcolo_do_checkpoint,
    .bdrv_stop_replication      = blkcolo_stop_replication,

    .supports_backing           = true,
    .has_variable_length        = true,
};

static void bdrv_blkcolo_init(void)
{
    bdrv_register(&bdrv_blkcolo);
};

block_init(bdrv_blkcolo_init);
