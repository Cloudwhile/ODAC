#!/bin/sh
# The crux: does the guard's conntrack rule drop a NEW connection?
#
# Everything is dumped, so the reading is direct: the rule that is
# installed, the counter it accumulated, the conntrack table's own view of
# the flow, and the connection's exit code.
set -u
[ "$(id -u)" -eq 0 ] || { echo 'run as root inside unshare -mn' >&2; exit 2; }
mount -t tmpfs tmpfs /run; mkdir -p /run/netns
mount -t sysfs sysfs /sys
ip netns add client; ip netns add inet
ip link add rtr0 type veth peer name c0 netns client
ip link add up0 type veth peer name i0 netns inet
ip link set lo up; ip link set rtr0 up; ip link set up0 up
ip addr add 10.250.5.1/24 dev rtr0; ip addr add 198.51.100.254/24 dev up0
ip netns exec client sh -c "ip link set lo up; ip link set c0 up; ip addr add 10.250.5.2/24 dev c0; ip route add default via 10.250.5.1"
ip netns exec inet sh -c "ip link set lo up; ip link set i0 up; ip addr add 198.51.100.1/24 dev i0; ip route add default via 198.51.100.254"
ip route add default via 198.51.100.1
echo 1 > /proc/sys/net/ipv4/ip_forward

echo "=== is conntrack tracking in this namespace? ==="
echo "# the counter before any traffic: $(cat /proc/sys/net/netfilter/nf_conntrack_count 2>&1)"
ip netns exec client ping -n -c1 -W1 198.51.100.1 >/dev/null 2>&1
echo "# the counter after a ping: $(cat /proc/sys/net/netfilter/nf_conntrack_count 2>&1)"
echo "# the table:"
cat /proc/net/nf_conntrack 2>&1 | sed 's/[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}/A/g'

echo
echo "=== the probe chain, with one rule at a time ==="
run_probe() {
    name="$1"; expr="$2"
    nft delete table inet probe 2>/dev/null
    nft -f - >/dev/null 2>&1 <<EOF
table inet probe {
    chain pc { type filter hook forward priority -30; policy accept; }
}
EOF
    nft add rule inet probe pc counter $expr >/dev/null 2>&1
    echo "--- $name"
    echo "  the rule as installed:"
    nft list chain inet probe pc 2>/dev/null | sed 's/^/    /'
    ip netns exec client curl -s --noproxy '*' --max-time 4 -o /dev/null \
        "http://198.51.100.1:9/" >/dev/null 2>&1
    rc=$?
    echo "  the connection's exit code: $rc"
    echo "  the chain after the traffic:"
    nft list chain inet probe pc 2>/dev/null | sed 's/^/    /'
    echo "  the conntrack table now:"
    cat /proc/net/nf_conntrack 2>/dev/null | sed 's/[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}/A/g' | sed 's/^/    /'
    nft delete table inet probe 2>/dev/null
}

run_probe 'ct state new' 'ct state new'
run_probe 'ct state established' 'ct state established'
run_probe 'ct state != { established, related }' 'ct state != { established, related }'

echo
echo "=== the same expression, but dropping ==="
nft delete table inet probe 2>/dev/null
nft -f - >/dev/null 2>&1 <<EOF
table inet probe {
    chain pc { type filter hook forward priority -30; policy accept; }
}
EOF
nft add rule inet probe pc ct state != { established, related } drop counter >/dev/null 2>&1
echo "  the rule as installed:"
nft list chain inet probe pc 2>/dev/null | sed 's/^/    /'
ip netns exec client curl -s --noproxy '*' --max-time 4 -o /dev/null \
    "http://198.51.100.1:9/" >/dev/null 2>&1
echo "  the connection's exit code: $?"
echo "  the chain after the traffic:"
nft list chain inet probe pc 2>/dev/null | sed 's/^/    /'
nft delete table inet probe 2>/dev/null
