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
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/netlink.h>

#define NETLINK_COLO 28

enum colo_netlink_op {
    COLO_QUERY_CHECKPOINT = (NLMSG_MIN_TYPE + 1),
    COLO_CHECKPOINT,
    COLO_FAILOVER,
    COLO_PROXY_INIT,
    COLO_PROXY_RESET, /* UNUSED, will be used for continuous FT */
};

typedef struct nic_device {
    NetClientState *nc;
    bool (*support_colo)(NetClientState *nc);
    int (*configure)(NetClientState *nc, bool up, int side, int index);
    QTAILQ_ENTRY(nic_device) next;
    bool is_up;
} nic_device;

typedef struct colo_msg {
    bool is_checkpoint;
} colo_msg;

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

    /* close netlink socket before cleanup tap device. */
    if (cp_info.sockfd >= 0) {
        close(cp_info.sockfd);
        cp_info.sockfd = -1;
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

static int colo_proxy_send(uint8_t *buff, uint64_t size, int type)
{
    struct sockaddr_nl sa;
    struct nlmsghdr msg;
    struct iovec iov;
    struct msghdr mh;
    int ret;

    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_pid = 0;
    sa.nl_groups = 0;

    msg.nlmsg_len = NLMSG_SPACE(0);
    msg.nlmsg_flags = NLM_F_REQUEST;
    if (type == COLO_PROXY_INIT) {
        msg.nlmsg_flags |= NLM_F_ACK;
    }
    msg.nlmsg_seq = 0;
    /* This is untrusty */
    msg.nlmsg_pid = cp_info.index;
    msg.nlmsg_type = type;

    iov.iov_base = &msg;
    iov.iov_len = msg.nlmsg_len;

    mh.msg_name = &sa;
    mh.msg_namelen = sizeof(sa);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = NULL;
    mh.msg_controllen = 0;
    mh.msg_flags = 0;

    ret = sendmsg(cp_info.sockfd, &mh, 0);
    if (ret <= 0) {
        error_report("can't send msg to kernel by netlink: %s",
                     strerror(errno));
    }

    return ret;
}

/* error: return -1, otherwise return 0 */
static int64_t colo_proxy_recv(uint8_t **buff, int flags)
{
    struct sockaddr_nl sa;
    struct iovec iov;
    struct msghdr mh = {
        .msg_name = &sa,
        .msg_namelen = sizeof(sa),
        .msg_iov = &iov,
        .msg_iovlen = 1,
    };
    uint8_t *tmp = g_malloc(16384);
    uint32_t size = 16384;
    int64_t len = 0;
    int ret;

    iov.iov_base = tmp;
    iov.iov_len = size;
next:
   ret = recvmsg(cp_info.sockfd, &mh, flags);
    if (ret <= 0) {
        goto out;
    }

    len += ret;
    if (mh.msg_flags & MSG_TRUNC) {
        size += 16384;
        tmp = g_realloc(tmp, size);
        iov.iov_base = tmp + len;
        iov.iov_len = size - len;
        goto next;
    }

    *buff = tmp;
    return len;

out:
    g_free(tmp);
    *buff = NULL;
    return ret;
}

int colo_proxy_init(int side)
{
    int skfd = 0;
    struct sockaddr_nl sa;
    struct nlmsghdr *h;
    struct timeval tv = {0, 500000}; /* timeout for recvmsg from kernel */
    int i = 1;
    int ret = -1;
    uint8_t *buff = NULL;
    int64_t size;

    skfd = socket(PF_NETLINK, SOCK_RAW, NETLINK_COLO);
    if (skfd < 0) {
        error_report("can not create a netlink socket: %s", strerror(errno));
        goto out;
    }
    cp_info.sockfd = skfd;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 0;
retry:
    sa.nl_pid = i++;

    if (i > 10) {
        error_report("netlink bind error");
        goto out;
    }

    ret = bind(skfd, (struct sockaddr *)&sa, sizeof(sa));
    if (ret < 0 && errno == EADDRINUSE) {
        error_report("colo index %d has already in used", sa.nl_pid);
        goto retry;
    }

    cp_info.index = sa.nl_pid;
    ret = colo_proxy_send(NULL, 0, COLO_PROXY_INIT);
    if (ret < 0) {
        goto out;
    }
    setsockopt(cp_info.sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ret = -1;
    size = colo_proxy_recv(&buff, 0);
    /* disable SO_RCVTIMEO */
    tv.tv_usec = 0;
    setsockopt(cp_info.sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (size < 0) {
        error_report("Can't recv msg from kernel by netlink: %s",
                     strerror(errno));
        goto out;
    }

    if (size) {
        h = (struct nlmsghdr *)buff;

        if (h->nlmsg_type == NLMSG_ERROR) {
            struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(h);
            if (size - sizeof(*h) < sizeof(*err)) {
                goto out;
            }
            ret = -err->error;
            if (ret) {
                goto out;
            }
        }
    }

    ret = configure_nic(side, cp_info.index);
    if (ret != 0) {
        error_report("excute colo-proxy-script failed");
    }
    colo_nic_side = side;

out:
    g_free(buff);
    return ret;
}

void colo_proxy_destroy(int side)
{
    if (cp_info.sockfd >= 0) {
        close(cp_info.sockfd);
    }
    teardown_nic(side, cp_info.index);
    cp_info.index = -1;
    colo_nic_side = -1;
}

int colo_proxy_failover(void)
{
    if (colo_proxy_send(NULL, 0, COLO_FAILOVER) < 0) {
        return -1;
    }

    return 0;
}

int colo_proxy_checkpoint(void)
{
    if (colo_proxy_send(NULL, 0, COLO_CHECKPOINT) < 0) {
        return -1;
    }

    return 0;
}

/*
do checkpoint: return 1
error: return -1
do not checkpoint: return 0
*/
int colo_proxy_compare(void)
{
    uint8_t *buff;
    int64_t size;
    struct nlmsghdr *h;
    struct colo_msg *m;
    int ret = -1;

    size = colo_proxy_recv(&buff, MSG_DONTWAIT);

    /* timeout, return no checkpoint message. */
    if (size <= 0) {
        return 0;
    }

    h = (struct nlmsghdr *) buff;

    if (h->nlmsg_type == NLMSG_ERROR) {
        goto out;
    }

    if (h->nlmsg_len < NLMSG_LENGTH(sizeof(*m))) {
        goto out;
    }

    m = NLMSG_DATA(h);

    ret = m->is_checkpoint ? 1 : 0;

out:
    g_free(buff);
    return ret;
}
