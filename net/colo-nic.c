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
#include "include/migration/migration.h"
#include "migration/migration-colo.h"
#include "net/net.h"
#include "net/colo-nic.h"
#include "qemu/error-report.h"


typedef struct nic_device {
    NetClientState *nc;
    bool (*support_colo)(NetClientState *nc);
    int (*configure)(NetClientState *nc, bool up, int side, int index);
    QTAILQ_ENTRY(nic_device) next;
    bool is_up;
} nic_device;


typedef struct colo_proxy {
    int sockfd;
    int index;
} colo_proxy;

static colo_proxy cp_info = {-1, -1};

QTAILQ_HEAD(, nic_device) nic_devices = QTAILQ_HEAD_INITIALIZER(nic_devices);
static int colo_nic_side = -1;

/*
* colo_proxy_script usage
* ./colo_proxy_script master/slave install/uninstall phy_if virt_if index
*/
static bool colo_nic_support(NetClientState *nc)
{
    return nc && nc->colo_script[0] && nc->colo_nicname[0];
}

static int launch_colo_script(char *argv[])
{
    int pid, status;
    char *script = argv[0];

    /* try to launch network script */
    pid = fork();
    if (pid == 0) {
        execv(script, argv);
        _exit(1);
    } else if (pid > 0) {
        while (waitpid(pid, &status, 0) != pid) {
            /* loop */
        }

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            return 0;
        }
    }
    return -1;
}

static int colo_nic_configure(NetClientState *nc,
            bool up, int side, int index)
{
    int i, argc = 6;
    char *argv[7], index_str[32];
    char **parg;

    if (!nc && index <= 0) {
        error_report("Can not parse colo_script or colo_nicname");
        return -1;
    }

    parg = argv;
    *parg++ = nc->colo_script;
    *parg++ = (char *)(side == COLO_SECONDARY_MODE ? "slave" : "master");
    *parg++ = (char *)(up ? "install" : "uninstall");
    *parg++ = nc->colo_nicname;
    *parg++ = nc->ifname;
    sprintf(index_str, "%d", index);
    *parg++ = index_str;
    *parg = NULL;

    for (i = 0; i < argc; i++) {
        if (!argv[i][0]) {
            error_report("Can not get colo_script argument");
            return -1;
        }
    }

    return launch_colo_script(argv);
}

static int configure_one_nic(NetClientState *nc,
             bool up, int side, int index)
{
    struct nic_device *nic;

    assert(nc);

    QTAILQ_FOREACH(nic, &nic_devices, next) {
        if (nic->nc == nc) {
            if (!nic->support_colo || !nic->support_colo(nic->nc)
                || !nic->configure) {
                return -1;
            }
            if (up == nic->is_up) {
                return 0;
            }

            if (nic->configure(nic->nc, up, side, index) && up) {
                return -1;
            }
            nic->is_up = up;
            return 0;
        }
    }

    return -1;
}

static int configure_nic(int side, int index)
{
    struct nic_device *nic;

    if (QTAILQ_EMPTY(&nic_devices)) {
        return -1;
    }

    QTAILQ_FOREACH(nic, &nic_devices, next) {
        if (configure_one_nic(nic->nc, 1, side, index)) {
            return -1;
        }
    }

    return 0;
}

static void teardown_nic(int side, int index)
{
    struct nic_device *nic;

    QTAILQ_FOREACH(nic, &nic_devices, next) {
        configure_one_nic(nic->nc, 0, side, index);
    }
}

void colo_add_nic_devices(NetClientState *nc)
{
    struct nic_device *nic = g_malloc0(sizeof(*nic));

    nic->support_colo = colo_nic_support;
    nic->configure = colo_nic_configure;
    /*
     * TODO
     * only support "-netdev tap,colo_scripte..."  options
     * "-net nic -net tap..." options is not supported
     */
    nic->nc = nc;

    QTAILQ_INSERT_TAIL(&nic_devices, nic, next);
}

void colo_remove_nic_devices(NetClientState *nc)
{
    struct nic_device *nic, *next_nic;

    if (!nc || colo_nic_side == -1) {
        return;
    }

    QTAILQ_FOREACH_SAFE(nic, &nic_devices, next, next_nic) {
        if (nic->nc == nc) {
            configure_one_nic(nc, 0, colo_nic_side, cp_info.index);
            QTAILQ_REMOVE(&nic_devices, nic, next);
            g_free(nic);
        }
    }
    colo_nic_side = -1;
}

int colo_proxy_init(int side)
{
    int ret = -1;

    ret = configure_nic(side, cp_info.index);
    if (ret != 0) {
        error_report("excute colo-proxy-script failed");
    }
    colo_nic_side = side;
    return ret;
}

void colo_proxy_destroy(int side)
{
    teardown_nic(side, cp_info.index);
    cp_info.index = -1;
    colo_nic_side = -1;
}
