#!/bin/sh
# The trace of a new TCP connection through the product's own guard.
#
# The guard's chain carries no counters, so its behaviour is observed with
# "nft monitor trace". The trace names every rule the kernel evaluates and
# the verdict of each, so the rule that handles the SYN is stated by the
# kernel itself rather than inferred from an exit code.
#
# The trace marker is inserted as the FIRST rule of the guard's chain, so
# the trace starts before the chain's own rules.
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

echo "=== the guard, installed by the product's script ==="
"$fw" init odactest0 10.250.5.1 2080 8443
"$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2 60
"$fw" gate odactest0 online 60
nft list chain inet odac_guard forward_guard 2>/dev/null | sed 's/^/  /'

echo
echo "=== the trace marker, as the first rule ==="
nft insert rule inet odac_guard forward_guard meta nftrace set 1 2>&1 | sed 's/^/  /'
echo "  the chain now:"
nft list chain inet odac_guard forward_guard 2>/dev/null | sed 's/^/  /'

echo
echo "=== the trace of a new TCP connection ==="
nft monitor trace > /tmp/odac-trace.out 2>/tmp/odac-trace.err &
mon=$!
sleep 2
ip netns exec client curl -s --noproxy '*' --max-time 5 -o /dev/null \
    "http://198.51.100.1:9/" >/dev/null 2>&1
echo "  the connection's exit code: $?"
sleep 2
kill "$mon" 2>/dev/null
wait "$mon" 2>/dev/null

echo "  the trace:"
cat /tmp/odac-trace.out 2>/dev/null | sed 's/^/    /'
echo "  the trace's errors:"
cat /tmp/odac-trace.err 2>/dev/null | sed 's/^/    /'

echo
echo "=== the conntrack table ==="
cat /proc/net/nf_conntrack 2>/dev/null \
    | sed 's/[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}/A/g' | sed 's/^/    /'
