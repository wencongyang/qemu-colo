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

#ifndef BLOCK_BLKCOLO_H
#define BLOCK_BLKCOLO_H

typedef struct disk_buffer {
    QSIMPLEQ_HEAD(, buffered_request_state) head;
} disk_buffer;

bool buffer_has_empty_range(disk_buffer *disk_buffer,
                            uint64_t sector, int nb_sectors);
void qiov_read_from_buffer(disk_buffer *disk_buffer, QEMUIOVector *qiov,
                           uint64_t sector, int nb_sectors);
void qiov_write_to_buffer(disk_buffer *disk_buffer, QEMUIOVector *qiov,
                          uint64_t sector, int nb_sectors, bool overwrite);
void flush_buffered_data_to_disk(disk_buffer *disk_buffer,
                                 BlockDriverState *bs);

void init_disk_buffer(disk_buffer *disk_buffer);
void clear_all_buffered_data(disk_buffer *disk_buffer);

#endif
