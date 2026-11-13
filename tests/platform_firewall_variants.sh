#!/bin/sh
# The last discrepancy, settled by varying ONE thing at a time.
#
# The trace of the chain the product's script installs said the SYN was
# accepted; the trace of the same rules, added one at a time, said the
# conntrack rule dropped it. The variants below use the same interface
# names, the same set definitions and the same rule text, and differ only
# in how the chain is built. A second marker at the end of the chain says
# whether the end was reached.
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

# The rules, exactly as the product writes them.
RULES='iifname "odactest0" meta nfproto ipv6 drop
oifname "odactest0" meta nfproto ipv6 drop
iifname "odactest0" ip daddr @upstream drop
oifname "odactest0" ip saddr @upstream drop
iifname "odactest0" ip daddr @blocked4 drop
oifname "odactest0" ip saddr @blocked4 drop
iifname "odactest0" iifname != @online drop
oifname "odactest0" oifname != @online drop
iifname "odactest0" ether saddr . ip saddr != @clients drop
oifname "odactest0" ip daddr != @replies drop
iifname "odactest0" ct state != { established, related } drop'

sets() {
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
    nft add element inet odac_guard clients { aa:bb:cc:dd:ee:01 . 10.250.5.2 timeout 60s } 2>/dev/null
    nft add element inet odac_guard replies { 10.250.5.2 timeout 60s } 2>/dev/null
    nft add element inet odac_guard online { "odactest0" timeout 60s } 2>/dev/null
}

capture() {
    label="$1"; port="$2"
    nft monitor trace > /tmp/v.out 2>/dev/null &
    mon=$!
    sleep 2
    ip netns exec client curl -s --noproxy '*' --max-time 5 -o /dev/null \
        "http://198.51.100.1:$port/" >/dev/null 2>&1
    rc=$?
    sleep 2
    kill "$mon" 2>/dev/null
    wait "$mon" 2>/dev/null
    echo "=== $label: the exit code is $rc"
    grep -E 'rule |policy |conntrack:' /tmp/v.out 2>/dev/null \
        | sed 's/^trace id [0-9a-f]* //' | sed 's/^/    /'
}

echo "############ V1: the product's own script ############"
"$fw" init odactest0 10.250.5.1 2080 8443
"$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2 60
"$fw" gate odactest0 online 60
nft insert rule inet odac_guard forward_guard meta nftrace set 1
nft add rule inet odac_guard forward_guard meta nftrace set 1
capture "V1: the product's script" 101

echo
echo "############ V2: the same rules, inline in the table block ############"
nft delete table inet odac_guard 2>/dev/null
nft -f - >/dev/null 2>&1 <<EOF
table inet odac_guard {
    set clients { type ether_addr . ipv4_addr; flags timeout; timeout 90s; size 1024; }
    set replies { type ipv4_addr; flags timeout; timeout 90s; size 1024; }
    set upstream { type ipv4_addr; }
    set blocked4 { type ipv4_addr; flags interval; elements = { 0.0.0.0/8, 10.0.0.0/8, 100.64.0.0/10, 127.0.0.0/8, 169.254.0.0/16, 172.16.0.0/12, 192.0.0.0/24, 192.168.0.0/16, 198.18.0.0/15, 224.0.0.0/4, 240.0.0.0/4 }; }
    set online { type ifname; flags timeout; timeout 90s; }
    chain forward_guard { type filter hook forward priority -10; policy accept;
$RULES
    }
}
EOF
nft add element inet odac_guard clients { aa:bb:cc:dd:ee:01 . 10.250.5.2 timeout 60s } 2>/dev/null
nft add element inet odac_guard replies { 10.250.5.2 timeout 60s } 2>/dev/null
nft add element inet odac_guard online { "odactest0" timeout 60s } 2>/dev/null
nft insert rule inet odac_guard forward_guard meta nftrace set 1
nft add rule inet odac_guard forward_guard meta nftrace set 1
capture "V2: the rules inline" 102

echo
echo "############ V3: the same rules, added one at a time ############"
nft delete table inet odac_guard 2>/dev/null
sets
printf '%s\n' "$RULES" | while IFS= read -r rule; do
    [ -n "$rule" ] || continue
    nft add rule inet odac_guard forward_guard "$rule" 2>/dev/null
done
nft insert rule inet odac_guard forward_guard meta nftrace set 1
nft add rule inet odac_guard forward_guard meta nftrace set 1
capture "V3: the rules added one at a time" 103
