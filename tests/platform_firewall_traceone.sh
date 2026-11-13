#!/bin/sh
# Why did the trace jump from the marker rule straight to the policy?
#
# The trace shows "meta nftrace set 1 (verdict continue)" and then
# "policy accept", with no other rule in between. The chain's rules are
# evaluated in order, so the rules between the marker and the policy must
# have been skipped, or the trace was cut short.
#
# The chain is replicated here with the trace marker and ONE rule at a
# time, and the trace says which rule produced the verdict. That identifies
# the rule that handles the SYN.
set -u
[ "$(id -u)" -eq 0 ] || { echo 'run as root inside unshare -mn' >&2; exit 2; }
mount -t tmpfs tmpfs /run; mkdir -p /run/netns
mount -t sysfs sysfs /sys
ip netns add client; ip netns add inet
ip link add rtr0 type veth peer name c0 netns client
ip link add up0 type veth peer name i0 netns inet
ip link set lo up; ip link set rtr0 up; ip link set up0 up
ip addr add 10.250.5.1/24 dev rtr0; ip addr add 198.51.100.254/24 dev up0
ip netns exec client sh -c "ip link set lo up; ip link set c0 address aa:bb:cc:dd:ee:01; ip link set c0 up; ip addr add 10.250.5.2/24 dev c0; ip route add default via 10.250.5.1"
ip netns exec inet sh -c "ip link set lo up; ip link set i0 up; ip addr add 198.51.100.1/24 dev i0; ip route add default via 198.51.100.254"
ip route add default via 198.51.100.1
echo 1 > /proc/sys/net/ipv4/ip_forward

# The replica, with the sets the product declares.
setup() {
    nft delete table inet rep 2>/dev/null
    nft -f - >/dev/null 2>&1 <<'EOF'
table inet rep {
    set clients { type ether_addr . ipv4_addr; }
    set replies { type ipv4_addr; }
    set upstream { type ipv4_addr; }
    set blocked4 { type ipv4_addr; flags interval; elements = { 0.0.0.0/8, 10.0.0.0/8, 100.64.0.0/10, 127.0.0.0/8, 169.254.0.0/16, 172.16.0.0/12, 192.0.0.0/24, 192.168.0.0/16, 198.18.0.0/15, 224.0.0.0/4, 240.0.0.0/4 }; }
    set online { type ifname; }
    chain fg { type filter hook forward priority -10; policy accept; }
}
EOF
    nft add element inet rep clients { aa:bb:cc:dd:ee:01 . 10.250.5.2 } 2>/dev/null
    nft add element inet rep replies { 10.250.5.2 } 2>/dev/null
    nft add element inet rep online { "rtr0" } 2>/dev/null
}

# trace_one NAME RULE...
trace_one() {
    name="$1"; shift
    setup
    nft insert rule inet rep fg meta nftrace set 1 >/dev/null 2>&1
    nft add rule inet rep fg "$@" >/dev/null 2>&1
    nft monitor trace > /tmp/t.out 2>/dev/null &
    mon=$!
    sleep 1
    ip netns exec client curl -s --noproxy '*' --max-time 4 -o /dev/null \
        "http://198.51.100.1:9/" >/dev/null 2>&1
    rc=$?
    sleep 1
    kill "$mon" 2>/dev/null
    wait "$mon" 2>/dev/null
    echo "--- $name (exit $rc)"
    # The verdict lines: which rule produced them.
    grep -E 'rule |policy |verdict' /tmp/t.out 2>/dev/null \
        | sed 's/^trace id [0-9a-f]* //' | sed 's/^/      /'
    nft delete table inet rep 2>/dev/null
}

echo "=== the product's rules, one at a time, each traced ==="
trace_one 'the conntrack rule alone' 'ct state != { established, related } drop'
trace_one 'the online rule alone' 'iifname != @online drop'
trace_one 'the oifname online rule alone' 'oifname != @online drop'
trace_one 'the clients rule alone' 'ether saddr . ip saddr != @clients drop'
trace_one 'the replies rule alone' 'ip daddr != @replies drop'
trace_one 'the blocked4 dest rule alone' 'ip daddr @blocked4 drop'
trace_one 'the blocked4 source rule alone' 'ip saddr @blocked4 drop'
trace_one 'the upstream dest rule alone' 'ip daddr @upstream drop'
