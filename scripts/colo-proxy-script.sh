#!/bin/sh
#usage: colo-proxy-script.sh master/slave install/uninstall phy_if virt_if index
#.e.g colo-proxy-script.sh master install eth2 tap0 1

side=$1
action=$2
phy_if=$3
virt_if=$4
index=$5
br=br1
failover_br=br0

script_usage()
{
    echo -n "usage: ./colo-proxy-script.sh master/slave "
    echo -e "install/uninstall phy_if virt_if index\n"
}

master_install()
{
    tc qdisc add dev $virt_if root handle 1: prio
    tc filter add dev $virt_if parent 1: protocol ip prio 10 u32 match u32 \
        0 0 flowid 1:2 action mirred egress mirror dev $phy_if
    tc filter add dev $virt_if parent 1: protocol arp prio 11 u32 match u32 \
        0 0 flowid 1:2 action mirred egress mirror dev $phy_if
    tc filter add dev $virt_if parent 1: protocol ipv6 prio 12 u32 match u32 \
        0 0 flowid 1:2 action mirred egress mirror dev $phy_if

    modprobe nf_conntrack_ipv4
    modprobe xt_PMYCOLO sec_dev=$phy_if

    /usr/local/sbin/iptables -t mangle -I PREROUTING -m physdev --physdev-in \
        $virt_if -j PMYCOLO --index $index
    /usr/local/sbin/ip6tables -t mangle -I PREROUTING -m physdev --physdev-in \
        $virt_if -j PMYCOLO --index $index
    /usr/local/sbin/arptables -I INPUT -i $phy_if -j MARK --set-mark $index
}

master_uninstall()
{
    tc filter del dev $virt_if parent 1: protocol ip prio 10 u32 match u32 \
        0 0 flowid 1:2 action mirred egress mirror dev $phy_if
    tc filter del dev $virt_if parent 1: protocol arp prio 11 u32 match u32 \
        0 0 flowid 1:2 action mirred egress mirror dev $phy_if
    tc filter del dev $virt_if parent 1: protocol ipv6 prio 12 u32 match u32 \
        0 0 flowid 1:2 action mirred egress mirror dev $phy_if
    tc qdisc del dev $virt_if root handle 1: prio

    /usr/local/sbin/iptables -t mangle -F
    /usr/local/sbin/ip6tables -t mangle -F
    /usr/local/sbin/arptables -F
    rmmod xt_PMYCOLO
}

slave_install()
{
    brctl addif $br $phy_if
    modprobe xt_SECCOLO

    /usr/local/sbin/iptables -t mangle -I PREROUTING -m physdev --physdev-in \
        $virt_if -j SECCOLO --index $index
    /usr/local/sbin/ip6tables -t mangle -I PREROUTING -m physdev --physdev-in \
        $virt_if -j SECCOLO --index $index
}

slave_uninstall()
{
    brctl delif $br $phy_if
    brctl delif $br $virt_if
    brctl addif $failover_br $virt_if

    /usr/local/sbin/iptables -t mangle -F
    /usr/local/sbin/ip6tables -t mangle -F
    rmmod xt_SECCOLO
}

if [ $# -ne 5 ]; then
    script_usage
    exit 1
fi

if [ "x$side" != "xmaster" ] && [ "x$side" != "xslave" ]; then
    script_usage
    exit 2
fi

if [ "x$action" != "xinstall" ] && [ "x$action" != "xuninstall" ]; then
    script_usage
    exit 3
fi

if [ $index -lt 0 ] || [ $index -gt 100 ]; then
    echo "index overflow"
    exit 4
fi

${side}_${action}
