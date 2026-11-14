#!/bin/sh
# The last discrepancy, resolved in one namespace.
#
# The trace of the chain the PRODUCT's script installed said the SYN was
# accepted. The trace of a replica of the same rules said the conntrack
# rule dropped it. Both cannot be true, so both are run here, in the same
# namespace, with the same interface names, and the raw traces are dumped.
#
# The chain listing is dumped immediately before the traffic, so the rules
# that were in force are known.
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

capture() {
    # capture LABEL : the trace and the exit code of one connection.
    label="$1"
    nft monitor trace > /tmp/raw.out 2>/dev/null &
    mon=$!
    sleep 2
    ip netns exec client curl -s --noproxy '*' --max-time 5 -o /dev/null \
        "http://198.51.100.1:9/" >/dev/null 2>&1
    rc=$?
    sleep 2
    kill "$mon" 2>/dev/null
    wait "$mon" 2>/dev/null
    echo "=== $label: the exit code is $rc"
    echo "--- the raw trace:"
    cat /tmp/raw.out 2>/dev/null
    echo "--- the end of the raw trace"
}

echo "############ A: the chain the product's script installs ############"
"$fw" init odactest0 10.250.5.1 2080 8443
"$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2 60
"$fw" gate odactest0 online 60
echo "--- the chain, before the traffic:"
nft list chain inet odac_guard forward_guard 2>/dev/null
echo "--- the sets:"
nft list sets table inet odac_guard 2>/dev/null
nft insert rule inet odac_guard forward_guard meta nftrace set 1 2>&1
echo "--- the chain, with the marker:"
nft list chain inet odac_guard forward_guard 2>/dev/null
capture "A: the product's chain"
echo "--- the chain, after the traffic:"
nft list chain inet odac_guard forward_guard 2>/dev/null

echo
echo "############ B: the same rules, in a replica table ############"
nft delete table inet odac_guard 2>/dev/null
nft -f - >/dev/null 2>&1 <<'EOF'
table inet odac_guard {
    set clients { type ether_addr . ipv4_addr; flags timeout; timeout 90s; size 1024; }
    set replies { type ipv4_addr; flags timeout; timeout 90s; size 1024; }
    set upstream { type ipv4_addr; }
    set blocked4 { type ipv4_addr; flags interval; elements = { 0.0.0.0/8, 10.0.0.0/8, 100.64.0.0/10, 127.0.0.0/8, 169.254.0.0/16, 172.16.0.0/12, 192.0.0.0/24, 192.168.0.0/16, 198.18.0.0/15, 224.0.0.0/4, 240.0.0.0/4 }; }
    set online { type ifname; flags timeout; timeout 90s; }
    chain forward_guard { type filter hook forward priority -10; policy accept; }
}
EOF
nft add element inet odac_guard clients { aa:bb:cc:dd:ee:01 . 10.250.5.2 timeout 60s }
nft add element inet odac_guard replies { 10.250.5.2 timeout 60s }
nft add element inet odac_guard online { "odactest0" timeout 60s }
nft insert rule inet odac_guard forward_guard meta nftrace set 1
echo "--- the chain, before the traffic:"
nft list chain inet odac_guard forward_guard 2>/dev/null
capture "B: the replica"
