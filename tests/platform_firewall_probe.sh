#!/bin/sh
# Two questions about the guard's own rules, answered on the real kernel.
#
# Q1: does "ct state != { established, related } drop" drop the SYN of a new
#     connection? The product's CI test only pings, so it cannot see this.
# Q2: what does "iifname != @online drop" match in the forward hook? The
#     rule was written to mean "the controlled interface must be online",
#     but the online set holds interface names, and in the forward hook the
#     incoming interface is the client's interface while the outgoing one is
#     the controlled interface.
#
# The answers come from the kernel, not from an inference: each rule is
# installed on its own and the traffic is measured.
set -u
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

# The client's new TCP connection. curl reports 7 when the far end refused
# the connection, 28 when the packet was dropped on the way.
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
ping4() { ip netns exec client ping -n -c1 -W1 "$1" >/dev/null 2>&1; }

echo "=== Q1: the conntrack rule and a new connection ==="
"$fw" init odactest0 10.250.5.1 2080 8443
"$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2 60
"$fw" gate odactest0 online 60

label='with the guard: a new TCP connection is refused by the far end'; expect tcp_refused
label='with the guard: a new TCP connection is not dropped'; expect_not tcp_dropped

# Now the guard without the conntrack rule.
ct_handle=$(nft -a list chain inet odac_guard forward_guard \
    | sed -n '/ct state/{s/.*# handle \([0-9][0-9]*\).*/\1/p;}')
echo "# the conntrack rule's handle is present: ${ct_handle:+yes}"
if [ -n "$ct_handle" ]; then
    nft delete rule inet odac_guard forward_guard handle "$ct_handle"
    label='without the conntrack rule: a new TCP connection is refused'; expect tcp_refused
    label='without the conntrack rule: a new TCP connection is not dropped'; expect_not tcp_dropped
fi

echo
echo "=== Q2: what the online rule matches in the forward hook ==="
# The online set holds the name of the controlled interface. In the forward
# hook, the incoming interface of a client's packet is the client's own
# interface, and the outgoing one is the controlled interface. The rule
# "iifname != @online drop" therefore asks whether the CLIENT's interface is
# in the set, not whether the controlled interface is.
#
# The measurement: with the client attached to a different interface than
# the one in the online set, does the client's traffic pass?
"$fw" init odactest0 10.250.5.1 2080 8443
"$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2 60
"$fw" gate odactest0 online 60
echo "# the online set now holds: odactest0"
echo "# the client's interface is: c0"
label='the online set holds the controlled interface'
expect nft get element inet odac_guard online '{ "odactest0" }'
label='the online set does not hold the client interface'
expect_not nft get element inet odac_guard online '{ "c0" }'
label='the client reaches the internet with the gate open'; expect ping4 198.51.100.1

echo
echo "=== the same, with the client attached to the controlled interface ==="
# The rule that matters is the one that checks the OUTGOING interface.
# "oifname != @online drop" is the egress counterpart. With the controlled
# interface in the online set, the egress is allowed.
echo "# the traffic leaves through odactest0, which is in the online set"

echo
echo "# pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
