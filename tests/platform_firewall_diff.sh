#!/bin/sh
# A byte level diff between the chain the product's script installs and the
# chain built from the same rule text by hand.
#
# The two behaved differently, so the rules themselves are compared, byte
# for byte, and any difference is reported.
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

echo "=== V1: the chain the product's script installs ==="
"$fw" init odactest0 10.250.5.1 2080 8443
"$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2 60
"$fw" gate odactest0 online 60
nft list chain inet odac_guard forward_guard 2>/dev/null | sed 's/^/  /'
echo "  --- the rule count:"
nft list chain inet odac_guard forward_guard 2>/dev/null \
    | grep -c 'drop\|accept\|return'
echo "  --- the ct rule, as the kernel stores it:"
nft list chain inet odac_guard forward_guard 2>/dev/null | grep 'ct state' | sed 's/^/  /'
echo "  --- the whole chain with its handles:"
nft -a list chain inet odac_guard forward_guard 2>/dev/null | sed 's/^/  /'

echo
echo "=== the rule text as the product's script writes it ==="
sed -n '/chain forward_guard/,/^    }/p' \
    "$here/../device-access/files/usr/libexec/odac-firewall" | sed 's/^/  /'
