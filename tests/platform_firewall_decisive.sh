#!/bin/sh
# The decisive measurement, with the PRODUCT's own script.
#
# Earlier runs disagreed, so everything is dumped here: the conntrack table
# between every step, the guard's chain as it stands, and the exit code of
# each connection. The whole sequence runs twice, so a result that depends
# on the order of the steps shows up as a difference between the runs.
#
# Nothing is replicated: the deployed script does the work.
set -u
here=$(cd "$(dirname "$0")" && pwd)
fw="$here/../device-access/files/usr/libexec/odac-firewall"
[ "$(id -u)" -eq 0 ] || { echo 'run as root inside unshare -mn' >&2; exit 2; }
mount -t tmpfs tmpfs /run; mkdir -p /run/netns
mount -t sysfs sysfs /sys
ip netns add client; ip netns add inet
ip link add odactest0 type veth peer name c0 netns client
ip link add up0 type veth peer name i0 netns inet
ip link set lo up; ip link set odactest0 up; ip link set up0 up
ip addr add 10.250.5.1/24 dev odactest0; ip addr add 198.51.100.254/24 dev up0
ip netns exec client sh -c "ip link set lo up; ip link set c0 address aa:bb:cc:dd:ee:01; ip link set c0 up; ip addr add 10.250.5.2/24 dev c0; ip route add default via 10.250.5.1"
ip netns exec inet sh -c "ip link set lo up; ip link set i0 up; ip addr add 198.51.100.1/24 dev i0; ip route add default via 198.51.100.254"
ip route add default via 198.51.100.1
echo 1 > /proc/sys/net/ipv4/ip_forward

dump() {
    echo "  the conntrack count: $(cat /proc/sys/net/netfilter/nf_conntrack_count 2>&1)"
    echo "  the conntrack table:"
    cat /proc/net/nf_conntrack 2>/dev/null \
        | sed 's/[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}/A/g' \
        | sed 's/^/      /'
}

try() {
    label="$1"
    ip netns exec client ping -n -c1 -W1 198.51.100.1 >/dev/null 2>&1
    prc=$?
    ip netns exec client curl -s --noproxy '*' --max-time 4 -o /dev/null \
        "http://198.51.100.1:9/" >/dev/null 2>&1
    crc=$?
    echo "  $label: the ping's exit code is $prc, the TCP exit code is $crc"
}

echo "=== does conntrack track in this namespace? ==="
echo "  the count before any traffic: $(cat /proc/sys/net/netfilter/nf_conntrack_count 2>&1)"
ip netns exec client ping -n -c1 -W1 198.51.100.1 >/dev/null 2>&1
echo "  the count after a ping: $(cat /proc/sys/net/netfilter/nf_conntrack_count 2>&1)"
echo "  the table after a ping:"
cat /proc/net/nf_conntrack 2>/dev/null \
    | sed 's/[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}/A/g' | sed 's/^/      /'
echo "  the conntrack module in this namespace:"
ls /sys/module/ 2>/dev/null | grep -i conntrack | sed 's/^/      /'
echo "  the conntrack max: $(cat /proc/sys/net/netfilter/nf_conntrack_max 2>&1)"

echo
echo "=== first pass: the product's script ==="
echo "### before the guard"
try "before the guard"
dump

echo "### the guard is installed, the client is authorised, the gate is open"
"$fw" init odactest0 10.250.5.1 2080 8443
"$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2 60
"$fw" gate odactest0 online 60
echo "  the guard's chain:"
nft list chain inet odac_guard forward_guard 2>/dev/null | sed 's/^/      /'
try "with the guard"
dump

echo "### the conntrack rule is removed"
ct_handle=$(nft -a list chain inet odac_guard forward_guard \
    | sed -n '/ct state/{s/.*# handle \([0-9]*\).*/\1/p;}')
echo "  the conntrack rule's handle: ${ct_handle:-none}"
nft delete rule inet odac_guard forward_guard handle "$ct_handle" 2>&1 | sed 's/^/  /'
try "without the conntrack rule"
dump

echo
echo "=== second pass: from a clean guard, the TCP connection first ==="
"$fw" init odactest0 10.250.5.1 2080 8443
"$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2 60
"$fw" gate odactest0 online 60
try "with the guard, TCP first"
dump

echo
echo "=== third pass: the guard with the conntrack rule, and only a TCP connection ==="
"$fw" init odactest0 10.250.5.1 2080 8443
"$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2 60
"$fw" gate odactest0 online 60
ip netns exec client curl -s --noproxy '*' --max-time 4 -o /dev/null \
    "http://198.51.100.1:9/" >/dev/null 2>&1
echo "  the TCP exit code is $?"
dump
