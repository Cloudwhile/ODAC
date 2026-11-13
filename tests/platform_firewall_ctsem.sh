#!/bin/sh
# How does the kernel evaluate the guard's conntrack rule?
#
# The guard's forward chain ends with
#
#   ct state != { established, related } drop
#
# A new connection's first packet is in the "new" state, so on its face the
# rule should drop it. The product's own test only pings, so it cannot see
# this. The measurement below asks the kernel directly: a rule with a
# counter is installed on its own, and the counter says how many packets
# matched.
#
# nft's exit status is not used to detect a rejected rule: this nft build
# does not always report one. The error output is checked instead.
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

pass=0; fail=0
ok() { pass=$((pass+1)); echo "ok - $1"; }
bad() { fail=$((fail+1)); echo "not ok - $1"; }

# probe NAME RULE_TEXT
# The rule is installed alone on a chain that only counts, so the counter
# says how many packets the expression matched.
probe() {
    name="$1"; rule="$2"
    nft delete table inet probe 2>/dev/null
    nft -f - >/dev/null 2>"$err" <<EOF
table inet probe {
    chain probechain { type filter hook forward priority -30; policy accept; }
}
EOF
    nft add rule inet probe probechain counter $rule >/dev/null 2>"$err"
    if grep -qi error "$err"; then
        echo "not ok - $name: the rule was rejected"; fail=$((fail+1)); return 1
    fi
    # A new TCP connection: the SYN is the packet in the "new" state.
    ip netns exec client curl -s --noproxy '*' --max-time 4 -o /dev/null \
        "http://198.51.100.1:9/" >/dev/null 2>&1
    rc=$?
    n=$(nft list chain inet probe probechain 2>/dev/null \
        | sed -n 's/.*counter packets \([0-9]*\).*/\1/p' | tail -1)
    [ -n "$n" ] || n=0
    echo "# $name: the rule matched $n packet(s), the connection's exit code was $rc"
    nft delete table inet probe 2>/dev/null
    echo "$n $rc" > "$result"
    return 0
}

err=/tmp/ctsem.err
result=/tmp/ctsem.res

echo "=== the conntrack state of a new connection ==="
probe 'ct state new' 'ct state new'
probe 'ct state established' 'ct state established'
probe 'ct state related' 'ct state related'
probe 'ct state invalid' 'ct state invalid'
probe 'ct state != { established, related }' 'ct state != { established, related }'

echo
echo "=== the conntrack table, with the addresses masked ==="
ip netns exec client curl -s --noproxy '*' --max-time 4 -o /dev/null \
    "http://198.51.100.1:9/" >/dev/null 2>&1
sed 's/[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}/A/g' \
    /proc/net/nf_conntrack 2>/dev/null

echo
echo "# pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
