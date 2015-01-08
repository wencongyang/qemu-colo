/*
 * QEMU block WAW
 *
 * Copyright Fujitsu, Corp. 2015
 * Copyright (c) 2015 Intel Corporation
 * Copyright (c) 2015 HUAWEI TECHNOLOGIES CO.,LTD.
 *
 * Authors:
 *  Wen Congyang (wency@cn.fujitsu.com)
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "block/block.h"

/* See if in-flight requests overlap and wait for them to complete */
void coroutine_fn
wait_for_overlapping_requests(CowJob *job, int64_t start, int64_t end)
{
    CowRequest *req;
    bool retry;

    do {
        retry = false;
        QLIST_FOREACH(req, &job->inflight_reqs, list) {
            if (end > req->start && start < req->end) {
                qemu_co_queue_wait(&req->wait_queue);
                retry = true;
                break;
            }
        }
    } while (retry);
}

/* Keep track of an in-flight request */
void cow_request_begin(CowRequest *req, CowJob *job,
                       int64_t start, int64_t end)
{
    req->start = start;
    req->end = end;
    qemu_co_queue_init(&req->wait_queue);
    QLIST_INSERT_HEAD(&job->inflight_reqs, req, list);
}

/* Forget about a completed request */
void cow_request_end(CowRequest *req)
{
    QLIST_REMOVE(req, list);
    qemu_co_queue_restart_all(&req->wait_queue);
}
