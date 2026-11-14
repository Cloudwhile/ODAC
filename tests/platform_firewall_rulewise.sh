#!/bin/sh
# Which rule of the guard's chain handles a new TCP connection?
#
# The previous measurement put a counter before every rule, but a counter
# before a drop still drops, so every rule matched. The rule that actually
# handles the packet is the one that DROPS it, so the rules are measured
# one at a time: each rule alone on the chain, and the connection's exit
# code says whether that rule dropped it.
#
# Exit 7 means the SYN reached the far end and was refused there. Exit 28
# means the SYN was dropped on the way.
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

base_chain() {
    nft delete table inet replica 2>/dev/null
    nft -f - >/dev/null 2>&1 <<'EOF'
table inet replica {
    set clients { type ether_addr . ipv4_addr; }
    set replies { type ipv4_addr; }
    set upstream { type ipv4_addr; }
    set blocked4 { type ipv4_addr; flags interval; elements = { 0.0.0.0/8, 10.0.0.0/8, 100.64.0.0/10, 127.0.0.0/8, 169.254.0.0/16, 172.16.0.0/12, 192.0.0.0/24, 192.168.0.0/16, 198.18.0.0/15, 224.0.0.0/4, 240.0.0.0/4 }; }
    chain fg { type filter hook forward priority -10; policy accept; }
}
EOF
    nft add element inet replica clients { aa:bb:cc:dd:ee:01 . 10.250.5.2 } 2>/dev/null
    nft add element inet replica replies { 10.250.5.2 } 2>/dev/null
}

# probe NAME RULE... : the rule alone on the chain.
probe() {
    name="$1"; shift
    base_chain
    nft add rule inet replica fg "$@" >/dev/null 2>&1 || {
        echo "not ok - $name: the rule was rejected"; return 1; }
    ip netns exec client curl -s --noproxy '*' --max-time 4 -o /dev/null \
        "http://198.51.100.1:9/" >/dev/null 2>&1
    rc=$?
    case "$rc" in
        7)  verdict='the SYN reached the far end (refused there): the rule did NOT drop it';;
        28) verdict='the SYN was dropped on the way: the rule DROPPED it';;
        *)  verdict='other outcome';;
    esac
    echo "# $name: exit $rc, $verdict"
    echo "    the rule: $*" | sed 's/"/'"'"'/g'
    nft list chain inet replica fg 2>/dev/null | sed -n 's/.*counter packets \([0-9]*\).*/    the rule matched \1 packet(s)/p'
}

echo "=== each rule of the guard's chain, alone ==="
probe 'rule 1: ipv6 on ingress'  iifname '"rtr0"' meta nfproto ipv6 drop
probe 'rule 2: ipv6 on egress'   oifname '"rtr0"' meta nfproto ipv6 drop
probe 'rule 3: upstream dest'   iifname '"rtr0"' ip daddr @upstream drop
probe 'rule 4: upstream source' oifname '"rtr0"' ip saddr @upstream drop
probe 'rule 5: blocked4 dest'   iifname '"rtr0"' ip daddr @blocked4 drop
probe 'rule 6: blocked4 source' oifname '"rtr0"' ip saddr @blocked4 drop
probe 'rule 7: the client set'  iifname '"rtr0"' ether saddr . ip saddr '!=' @clients drop
probe 'rule 8: the replies set' oifname '"rtr0"' ip daddr '!=' @replies drop
probe 'rule 9: the conntrack rule' iifname '"rtr0"' ct state '!=' '{ established, related }' drop

echo
echo "=== the whole chain, in the product's order ==="
base_chain
nft add rule inet replica fg iifname '"rtr0"' meta nfproto ipv6 drop >/dev/null 2>&1
nft add rule inet replica fg oifname '"rtr0"' meta nfproto ipv6 drop >/dev/null 2>&1
nft add rule inet replica fg iifname '"rtr0"' ip daddr @upstream drop >/dev/null 2>&1
nft add rule inet replica fg oifname '"rtr0"' ip saddr @upstream drop >/dev/null 2>&1
nft add rule inet replica fg iifname '"rtr0"' ip daddr @blocked4 drop >/dev/null 2>&1
nft add rule inet replica fg oifname '"rtr0"' ip saddr @blocked4 drop >/dev/null 2>&1
nft add rule inet replica fg iifname '"rtr0"' ether saddr . ip saddr '!=' @clients drop >/dev/null 2>&1
nft add rule inet replica fg oifname '"rtr0"' ip daddr '!=' @replies drop >/dev/null 2>&1
nft add rule inet replica fg iifname '"rtr0"' ct state '!=' '{ established, related }' drop >/dev/null 2>&1
ip netns exec client curl -s --noproxy '*' --max-time 4 -o /dev/null \
    "http://198.51.100.1:9/" >/dev/null 2>&1
echo "# with the whole chain, the connection's exit code is: $?"

echo
echo "=== the whole chain, but with the conntrack rule removed ==="
nft delete rule inet replica fg handle "$(
    nft -a list chain inet replica fg | sed -n '/ct state/{s/.*# handle \([0-9]*\).*/\1/p;}')" >/dev/null 2>&1
ip netns exec client curl -s --noproxy '*' --max-time 4 -o /dev/null \
    "http://198.51.100.1:9/" >/dev/null 2>&1
echo "# without the conntrack rule, the connection's exit code is: $?"
