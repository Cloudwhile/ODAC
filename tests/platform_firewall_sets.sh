#!/bin/sh
# Which set makes the difference?
#
# The trace of the product's chain shows the conntrack rule dropping the
# SYN. The trace of the deployed script's chain showed the SYN being
# accepted. The chains are the same text, so the difference is in the SETS.
#
# The deployed script adds the client to "clients" and to "replies" and puts
# the controlled interface into "online". This script walks through those
# sets one at a time and traces each time, so the set that changes the
# outcome is identified by the kernel.
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

# The sets, declared exactly as the product declares them, including the
# flags and the timeout.
nft -f - >/dev/null 2>&1 <<'EOF'
table inet rep {
    set clients { type ether_addr . ipv4_addr; flags timeout; timeout 90s; size 1024; }
    set replies { type ipv4_addr; flags timeout; timeout 90s; size 1024; }
    set upstream { type ipv4_addr; }
    set blocked4 { type ipv4_addr; flags interval; elements = { 0.0.0.0/8, 10.0.0.0/8, 100.64.0.0/10, 127.0.0.0/8, 169.254.0.0/16, 172.16.0.0/12, 192.0.0.0/24, 192.168.0.0/16, 198.18.0.0/15, 224.0.0.0/4, 240.0.0.0/4 }; }
    set online { type ifname; flags timeout; timeout 90s; }
    chain fg { type filter hook forward priority -10; policy accept; }
}
EOF

build_chain() {
    nft flush chain inet rep fg 2>/dev/null
    nft insert rule inet rep fg meta nftrace set 1 >/dev/null 2>&1
    nft add rule inet rep fg iifname '"rtr0"' meta nfproto ipv6 drop >/dev/null 2>&1
    nft add rule inet rep fg oifname '"rtr0"' meta nfproto ipv6 drop >/dev/null 2>&1
    nft add rule inet rep fg iifname '"rtr0"' ip daddr @upstream drop >/dev/null 2>&1
    nft add rule inet rep fg oifname '"rtr0"' ip saddr @upstream drop >/dev/null 2>&1
    nft add rule inet rep fg iifname '"rtr0"' ip daddr @blocked4 drop >/dev/null 2>&1
    nft add rule inet rep fg oifname '"rtr0"' ip saddr @blocked4 drop >/dev/null 2>&1
    nft add rule inet rep fg iifname '"rtr0"' iifname '!=' @online drop >/dev/null 2>&1
    nft add rule inet rep fg oifname '"rtr0"' oifname '!=' @online drop >/dev/null 2>&1
    nft add rule inet rep fg iifname '"rtr0"' ether saddr . ip saddr '!=' @clients drop >/dev/null 2>&1
    nft add rule inet rep fg oifname '"rtr0"' ip daddr '!=' @replies drop >/dev/null 2>&1
    nft add rule inet rep fg iifname '"rtr0"' ct state '!=' '{ established, related }' drop >/dev/null 2>&1
}

run() {
    label="$1"
    nft monitor trace > /tmp/s.out 2>/dev/null &
    mon=$!
    sleep 1
    ip netns exec client curl -s --noproxy '*' --max-time 4 -o /dev/null \
        "http://198.51.100.1:9/" >/dev/null 2>&1
    rc=$?
    sleep 1
    kill "$mon" 2>/dev/null
    wait "$mon" 2>/dev/null
    echo "--- $label: the exit code is $rc"
    grep -oE 'rule [^ ]+.*\(verdict [a-z]+\)' /tmp/s.out 2>/dev/null \
        | grep -v nftrace | sed 's/^/      /' | head -3
}

echo "=== step 1: no elements in any set ==="
build_chain
run "no elements"

echo
echo "=== step 2: the client is in @clients ==="
nft add element inet rep clients { aa:bb:cc:dd:ee:01 . 10.250.5.2 timeout 60s } 2>/dev/null
run "the client in @clients"

echo
echo "=== step 3: the client is in @replies as well ==="
nft add element inet rep replies { 10.250.5.2 timeout 60s } 2>/dev/null
run "the client in @clients and @replies"

echo
echo "=== step 4: the controlled interface is in @online ==="
nft add element inet rep online { "rtr0" timeout 60s } 2>/dev/null
run "the client in @clients and @replies, the interface in @online"

echo
echo "=== the sets as they stand ==="
nft list sets table inet rep 2>/dev/null | sed 's/^/    /'
