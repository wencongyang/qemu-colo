/*
 * Block driver for COLO
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

#include "qemu-common.h"
#include "qemu/queue.h"
#include "block/block.h"
#include "block/blkcolo.h"

typedef struct buffered_request_state {
    uint64_t start_sector;
    int nb_sectors;
    void *data;
    QSIMPLEQ_ENTRY(buffered_request_state) entry;
} buffered_request_state;

/* common functions */
/*
 * The buffered data may eat too much memory, and glibc cannot work
 * very well in such case.
 */
static void *alloc_buffered_data(int nb_sectors)
{
    return g_malloc(nb_sectors * BDRV_SECTOR_SIZE);
}

static void free_buffered_data(void *data)
{
    g_free(data);
}

typedef struct search_brs_state {
    uint64_t sector;
    buffered_request_state *prev;
} search_brs_state;

static buffered_request_state *search_brs(disk_buffer *disk_buffer,
                                           search_brs_state *sbs)
{
    buffered_request_state *brs;

    QSIMPLEQ_FOREACH(brs, &disk_buffer->head, entry) {
        if (sbs->sector < brs->start_sector) {
            return NULL;
        }

        if (sbs->sector < brs->start_sector + brs->nb_sectors) {
            return brs;
        }

        sbs->prev = brs;
    }

    return NULL;
}

static buffered_request_state *get_next_brs(buffered_request_state *brs)
{
    return QSIMPLEQ_NEXT(brs, entry);
}

static void add_brs_after(disk_buffer *disk_buffer,
                          buffered_request_state *new_brs,
                          buffered_request_state *prev)
{
    if (!prev) {
        QSIMPLEQ_INSERT_HEAD(&disk_buffer->head, new_brs, entry);
    } else {
        QSIMPLEQ_INSERT_AFTER(&disk_buffer->head, prev, new_brs, entry);
    }
}

static bool disk_buffer_empty(disk_buffer *disk_buffer)
{
    return QSIMPLEQ_EMPTY(&disk_buffer->head);
}

/* Disk buffer */
static buffered_request_state *create_new_brs(QEMUIOVector *qiov,
                                              uint64_t iov_sector,
                                              uint64_t sector, int nb_sectors)
{
    buffered_request_state *brs;

    brs = g_slice_new(buffered_request_state);
    brs->start_sector = sector;
    brs->nb_sectors = nb_sectors;
    brs->data = alloc_buffered_data(nb_sectors);
    qemu_iovec_to_buf(qiov, (sector - iov_sector) * BDRV_SECTOR_SIZE,
                      brs->data, nb_sectors * BDRV_SECTOR_SIZE);

    return brs;
}

static void free_brs(buffered_request_state *brs)
{
    free_buffered_data(brs->data);
    g_slice_free(buffered_request_state, brs);
}

bool buffer_has_empty_range(disk_buffer *disk_buffer,
                            uint64_t sector, int nb_sectors)
{
    buffered_request_state *brs;
    search_brs_state sbs;
    uint64_t cur_sector = sector;

    if (nb_sectors <= 0) {
        return false;
    }

    sbs.sector = sector;
    sbs.prev = NULL;
    brs = search_brs(disk_buffer, &sbs);
    if (!brs) {
        return true;
    }

    while (brs && cur_sector < sector + nb_sectors) {
        if (cur_sector < brs->start_sector) {
            return true;
        }

        if (brs->start_sector + brs->nb_sectors >= sector + nb_sectors) {
            return false;
        }

        cur_sector = brs->start_sector + brs->nb_sectors;
        brs = get_next_brs(brs);
    }

    if (cur_sector < sector + nb_sectors) {
        return true;
    } else {
        return false;
    }
}

/* Note: only the sector that exists in the buffer will be overwriten */
void qiov_read_from_buffer(disk_buffer *disk_buffer, QEMUIOVector *qiov,
                           uint64_t sector, int nb_sectors)
{
    search_brs_state sbs;
    buffered_request_state *brs;
    size_t offset, cur_nb_sectors;
    uint64_t cur_sector = sector;
    void *buf;

    if (disk_buffer_empty(disk_buffer)) {
        /* The disk buffer is empty */
        return;
    }

    sbs.sector = sector;
    sbs.prev = NULL;
    brs = search_brs(disk_buffer, &sbs);
    if (!brs) {
        if (!sbs.prev) {
            brs = QSIMPLEQ_FIRST(&disk_buffer->head);
        } else {
            brs = get_next_brs(sbs.prev);
        }
    }

    while (brs && cur_sector < sector + nb_sectors) {
        if (brs->start_sector >= sector + nb_sectors) {
            break;
        }

        /* In the first loop, brs->start_sector can be less than sector */
        if (brs->start_sector < cur_sector) {
            offset = cur_sector - brs->start_sector;
            buf = brs->data + offset * BDRV_SECTOR_SIZE;
        } else {
            cur_sector = brs->start_sector;
            offset = 0;
            buf = brs->data;
        }
        if (brs->start_sector + brs->nb_sectors >= sector + nb_sectors) {
            cur_nb_sectors = sector + nb_sectors - cur_sector;
        } else {
            cur_nb_sectors = brs->nb_sectors - offset;
        }
        qemu_iovec_from_buf(qiov, (cur_sector - sector) * BDRV_SECTOR_SIZE,
                            buf, cur_nb_sectors * BDRV_SECTOR_SIZE);

        cur_sector = brs->start_sector + brs->nb_sectors;
        brs = get_next_brs(brs);
    }
}

void qiov_write_to_buffer(disk_buffer *disk_buffer, QEMUIOVector *qiov,
                          uint64_t sector, int nb_sectors, bool overwrite)
{
    search_brs_state sbs;
    buffered_request_state *brs, *new_brs, *prev;
    uint64_t cur_sector = sector;
    int cur_nb_sectors, offset;

    if (disk_buffer_empty(disk_buffer)) {
        /* The disk buffer is empty */
        new_brs = create_new_brs(qiov, sector, cur_sector, nb_sectors);
        add_brs_after(disk_buffer, new_brs, NULL);
        return;
    }

    sbs.sector = sector;
    sbs.prev = NULL;
    brs = search_brs(disk_buffer, &sbs);
    if (!sbs.prev) {
        prev = NULL;
        brs = QSIMPLEQ_FIRST(&disk_buffer->head);
    } else {
        prev = sbs.prev;
        brs = get_next_brs(sbs.prev);
    }

    while (brs && cur_sector < sector + nb_sectors) {
        if (cur_sector < brs->start_sector) {
            if (sector + nb_sectors <= brs->start_sector) {
                cur_nb_sectors = sector + nb_sectors - cur_sector;
            } else {
                cur_nb_sectors = brs->start_sector - cur_sector;
            }
            new_brs = create_new_brs(qiov, sector, cur_sector, cur_nb_sectors);
            add_brs_after(disk_buffer, new_brs, prev);
            cur_sector = brs->start_sector;
        }

        if (cur_sector >= sector + nb_sectors) {
            break;
        }

        if (overwrite) {
            offset = cur_sector - brs->start_sector;
            if (sector + nb_sectors <= brs->start_sector + brs->nb_sectors) {
                cur_nb_sectors = sector + nb_sectors - cur_sector;
            } else {
                cur_nb_sectors = brs->nb_sectors - offset;
            }
            qemu_iovec_to_buf(qiov, (cur_sector - sector) * BDRV_SECTOR_SIZE,
                              brs->data + offset * BDRV_SECTOR_SIZE,
                              cur_nb_sectors * BDRV_SECTOR_SIZE);
        }

        cur_sector = brs->start_sector + brs->nb_sectors;

        prev = brs;
        brs = get_next_brs(brs);
    }

    if (cur_sector < sector + nb_sectors) {
        new_brs = create_new_brs(qiov, sector, cur_sector,
                                 sector + nb_sectors - cur_sector);
        add_brs_after(disk_buffer, new_brs, prev);
    }
}

struct flushed_data {
    QEMUIOVector qiov;
    buffered_request_state *brs;
};

static void flush_buffered_data_complete(void *opaque, int ret)
{
    struct flushed_data *flushed_data = opaque;

    /* We have reported the guest that this write ops successed */
    assert(ret == 0);

    qemu_iovec_destroy(&flushed_data->qiov);
    free_brs(flushed_data->brs);
    g_free(flushed_data);
}

void flush_buffered_data_to_disk(disk_buffer *disk_buffer,
                                 BlockDriverState *bs)
{
    buffered_request_state *brs, *tmp;
    struct flushed_data *flushed_data = NULL;

    QSIMPLEQ_FOREACH_SAFE(brs, &disk_buffer->head, entry, tmp) {
        /* brs is always the head */
        QSIMPLEQ_REMOVE_HEAD(&disk_buffer->head, entry);

        flushed_data = g_malloc(sizeof(struct flushed_data));
        qemu_iovec_init(&flushed_data->qiov, 1);
        qemu_iovec_add(&flushed_data->qiov, brs->data,
                       brs->nb_sectors * BDRV_SECTOR_SIZE);
        flushed_data->brs = brs;
        bdrv_aio_writev(bs, brs->start_sector, &flushed_data->qiov,
                        brs->nb_sectors, flush_buffered_data_complete,
                        flushed_data);
    }

    bdrv_drain_all();
}

void init_disk_buffer(disk_buffer *disk_buffer)
{
    QSIMPLEQ_INIT(&disk_buffer->head);
}

void clear_all_buffered_data(disk_buffer *disk_buffer)
{
    buffered_request_state *brs, *tmp;

    QSIMPLEQ_FOREACH_SAFE(brs, &disk_buffer->head, entry, tmp) {
        /* brs is always the head */
        QSIMPLEQ_REMOVE_HEAD(&disk_buffer->head, entry);
        free_brs(brs);
    }
}
