#!/bin/sh
# Does the guard allow a NEW TCP connection from an authorised client?
#
# The existing test only pings. An ICMP echo request and the SYN of a new
# TCP connection are different conntrack states, so a ping-based test cannot
# answer this question. The guard's forward chain ends with
#
#   ct state != { established, related } drop
#
# which drops every packet that is not part of an established flow, including
# the SYN of a new connection.
#
# This test answers the question on the real kernel:
#   A: the guard exactly as deployed
#   B: the same guard with only the conntrack rule removed
#
# curl to a closed port distinguishes the two outcomes. Exit 7 means the SYN
# reached the far end and was refused there; exit 28 means it was dropped on
# the way. The A/B identifies the rule that made the difference.
#
# Run as   unshare -rnm sh tests/platform_firewall_tcp.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
fw="$here/../device-access/files/usr/libexec/odac-firewall"
[ "$(id -u)" -eq 0 ] || { echo 'run inside unshare -rnm' >&2; exit 2; }
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
# A permissive "fw4" stand-in at priority 0 accepts everything: proves our
# drops win.
nft -f - <<'EOF'
table inet fw4 {
    chain forward { type filter hook forward priority 0; policy accept; ct state established,related accept; accept; }
    chain input { type filter hook input priority 0; policy accept; }
}
EOF
pass=0; fail=0
ok() { pass=$((pass+1)); echo "ok - $1"; }
bad() { fail=$((fail+1)); echo "not ok - $1"; }
expect() { if "$@" >/dev/null 2>&1; then ok "$label"; else bad "$label"; fi; }
expect_not() { if "$@" >/dev/null 2>&1; then bad "$label"; else ok "$label"; fi; }

# A new TCP connection, from the client to a port nobody listens on. curl
# reports 7 when the far end refused the connection and 28 when the packet
# was dropped. Only the exit code is used, never the text.
tcp_refused() {
    ip netns exec client curl -s --noproxy '*' --max-time 3 -o /dev/null \
        "http://198.51.100.1:9/" >/dev/null 2>&1
    [ "$?" -eq 7 ]
}
tcp_dropped() {
    ip netns exec client curl -s --noproxy '*' --max-time 3 -o /dev/null \
        "http://198.51.100.1:9/" >/dev/null 2>&1
    [ "$?" -eq 28 ]
}

label='no guard: baseline TCP connection reaches the far end'; expect tcp_refused

"$fw" init odactest0 10.250.5.1 2080 8443
"$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2 60
"$fw" gate odactest0 online 60

label='guard as deployed: a new TCP connection is allowed'; expect tcp_refused
label='guard as deployed: a new TCP connection is not dropped'; expect_not tcp_dropped

# --- the A/B: remove only the conntrack rule --------------------------------
ct_handle=$(nft -a list chain inet odac_guard forward_guard \
    | sed -n '/ct state/{s/.*# handle \([0-9][0-9]*\).*/\1/p;}')
ct_present() { nft list chain inet odac_guard forward_guard | grep -q 'ct state'; }
if [ -n "$ct_handle" ]; then
    nft delete rule inet odac_guard forward_guard handle "$ct_handle"
    label='the conntrack rule is gone'; expect_not ct_present
    label='without the conntrack rule: a new TCP connection is allowed'; expect tcp_refused
    label='without the conntrack rule: a new TCP connection is not dropped'; expect_not tcp_dropped
else
    label='the conntrack rule is present'; bad 'the conntrack rule is present'
fi

echo "# pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
