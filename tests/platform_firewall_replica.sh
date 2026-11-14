#!/bin/sh
# Which of the guard's rules handles a NEW TCP connection?
#
# The guard's forward chain is replicated here rule by rule, each rule with
# its own counter, and the counters say which rule handled each packet.
# The chain's text is the product's own, copied from the deployed script.
#
# The replication is necessary because the product's chain carries no
# counters, so the live table cannot answer this.
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

echo "=== the conntrack table before any traffic ==="
echo "# the count: $(cat /proc/sys/net/netfilter/nf_conntrack_count 2>&1)"

# The guard's forward chain, replicated with a counter on each rule. The
# sets are declared exactly as the product declares them.
nft -f - >/dev/null 2>&1 <<'EOF'
table inet replica {
    set clients { type ether_addr . ipv4_addr; }
    set replies { type ipv4_addr; }
    set upstream { type ipv4_addr; }
    set blocked4 { type ipv4_addr; flags interval; elements = { 0.0.0.0/8, 10.0.0.0/8, 100.64.0.0/10, 127.0.0.0/8, 169.254.0.0/16, 172.16.0.0/12, 192.0.0.0/24, 192.168.0.0/16, 198.18.0.0/15, 224.0.0.0/4, 240.0.0.0/4 }; }
    chain fg { type filter hook forward priority -10; policy accept; }
}
EOF
echo "# the replica's table:"
nft list table inet replica 2>&1 | sed 's/^/    /'

add() { nft add rule inet replica fg counter "$@" >/dev/null 2>&1; }
add iifname '"rtr0"' meta nfproto ipv6 drop
add oifname '"rtr0"' meta nfproto ipv6 drop
add iifname '"rtr0"' ip daddr @upstream drop
add oifname '"rtr0"' ip saddr @upstream drop
add iifname '"rtr0"' ip daddr @blocked4 drop
add oifname '"rtr0"' ip saddr @blocked4 drop
add iifname '"rtr0"' ether saddr . ip saddr != @clients drop
add oifname '"rtr0"' ip daddr != @replies drop
add iifname '"rtr0"' ct state != '{ established, related }' drop

# The client is authorised, exactly as the product's auth does it.
nft add element inet replica clients { aa:bb:cc:dd:ee:01 . 10.250.5.2 } 2>/dev/null
nft add element inet replica replies { 10.250.5.2 } 2>/dev/null

echo
echo "=== the replicated chain ==="
nft list chain inet replica fg 2>&1 | sed 's/^/    /'

echo
echo "=== a new TCP connection ==="
ip netns exec client curl -s --noproxy '*' --max-time 4 -o /dev/null \
    "http://198.51.100.1:9/" >/dev/null 2>&1
echo "# the connection's exit code: $?"
echo "# the conntrack count afterwards: $(cat /proc/sys/net/netfilter/nf_conntrack_count 2>&1)"
echo "# the conntrack table:"
cat /proc/net/nf_conntrack 2>/dev/null | sed 's/[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}/A/g' | sed 's/^/    /'

echo
echo "=== the counters after the connection ==="
nft list chain inet replica fg 2>&1 | sed 's/^/    /'

echo
echo "=== the same, with a ping ==="
ip netns exec client ping -n -c1 -W1 198.51.100.1 >/dev/null 2>&1
echo "# the ping's exit code: $?"
nft list chain inet replica fg 2>&1 | sed 's/^/    /'
