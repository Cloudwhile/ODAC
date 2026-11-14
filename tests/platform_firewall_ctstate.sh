#!/bin/sh
# Does the guard's conntrack rule break NEW connections?
#
# The product's existing test only pings, and it runs in an unprivileged
# user namespace, where /proc/net/nf_conntrack is not readable and the
# conntrack table of the namespace stays empty. A ping therefore says
# nothing about a new TCP connection.
#
# This test runs in a root-owned network namespace, where conntrack really
# tracks the flows, and it asks the kernel directly: the connection's exit
# code says whether the SYN reached the far end (7) or was dropped (28).
#
# The A/B removes only the conntrack rule, so the rule that made the
# difference is identified.
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
ping4() { ip netns exec client ping -n -c1 -W1 "$1" >/dev/null 2>&1; }

# curl reports 7 when the far end refused the connection and 28 when the
# packet was dropped on the way.
tcp_rc() {
    ip netns exec client curl -s --noproxy '*' --max-time 4 -o /dev/null \
        "http://198.51.100.1:9/" >/dev/null 2>&1
    echo "$?"
}
tcp_refused() { [ "$(tcp_rc)" = 7 ]; }
tcp_dropped() { [ "$(tcp_rc)" = 28 ]; }

echo "# the conntrack table is readable here: $(wc -l < /proc/net/nf_conntrack 2>/dev/null || echo 0) entries"

# --- the baseline, with no guard --------------------------------------------
label='no guard: a ping reaches the far end'; expect ping4 198.51.100.1
base_rc=$(tcp_rc)
echo "# with no guard, the TCP exit code is: $base_rc"
echo "# the conntrack table now holds: $(wc -l < /proc/net/nf_conntrack 2>/dev/null || echo 0) entries"

# --- the guard, exactly as deployed -----------------------------------------
"$fw" init odactest0 10.250.5.1 2080 8443
"$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2 60
"$fw" gate odactest0 online 60

label='with the guard: a ping reaches the far end'; expect ping4 198.51.100.1
a_rc=$(tcp_rc)
echo "# with the guard, the TCP exit code is: $a_rc"
echo "# the conntrack table now holds: $(wc -l < /proc/net/nf_conntrack 2>/dev/null || echo 0) entries"

label='with the guard: a new TCP connection is refused by the far end'; expect tcp_refused
label='with the guard: a new TCP connection is not dropped'; expect_not tcp_dropped

# --- the A/B: remove only the conntrack rule --------------------------------
ct_handle=$(nft -a list chain inet odac_guard forward_guard \
    | sed -n '/ct state/{s/.*# handle \([0-9][0-9]*\).*/\1/p;}')
echo "# the conntrack rule's handle is: ${ct_handle:-none}"
if [ -n "$ct_handle" ]; then
    nft delete rule inet odac_guard forward_guard handle "$ct_handle"
    b_rc=$(tcp_rc)
    echo "# without the conntrack rule, the TCP exit code is: $b_rc"
    label='without the conntrack rule: a new TCP connection is refused'; expect tcp_refused
    label='without the conntrack rule: a new TCP connection is not dropped'; expect_not tcp_dropped
    if [ "$a_rc" = "$b_rc" ]; then
        ok 'the conntrack rule does not change the outcome'
    else
        bad 'the conntrack rule changes the outcome'
    fi
fi

echo "# pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
