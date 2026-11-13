#!/bin/sh
# What conntrack state does a new TCP connection have while the guard's
# conntrack rule is in force?
#
# The rule is "ct state != { established, related } drop". A new connection
# starts in the "new" state, so the rule must drop it unless something else
# lets it through. This test watches the conntrack table while an authorised
# client opens a connection, and reads the table from /proc, which needs no
# extra tool.
#
# Run as   unshare -rnm sh tests/platform_firewall_conntrack.sh
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

# The conntrack table, from /proc. Only the protocol, the state and the
# ports are read; the addresses are not printed.
ct_states() {
    sed -n 's/.*\b\([A-Z_]*\)\b.*/\1/p' /proc/net/nf_conntrack 2>/dev/null \
        | sort -u | tr '\n' ' '
}
ct_has_new() { grep -q '\[UNREPLIED\]' /proc/net/nf_conntrack 2>/dev/null; }
ct_has_established() { grep -q 'ESTABLISHED' /proc/net/nf_conntrack 2>/dev/null; }

label='conntrack is tracking in this namespace'; expect test -s /proc/net/nf_conntrack
echo "# the conntrack table has $(wc -l < /proc/net/nf_conntrack 2>/dev/null || echo 0) entries"

"$fw" init odactest0 10.250.5.1 2080 8443
"$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2 60
"$fw" gate odactest0 online 60

# A new TCP connection, and the conntrack entry it creates.
ip netns exec client curl -s --noproxy '*' --max-time 3 -o /dev/null \
    "http://198.51.100.1:9/" >/dev/null 2>&1
rc=$?
echo "# the connection's exit code: $rc"
echo "# the conntrack states present: $(ct_states)"
label='the connection left a conntrack entry'; expect test -s /proc/net/nf_conntrack
label='the conntrack entry is in the new or established state'
expect sh -c 'grep -q "UNREPLIED\|ESTABLISHED" /proc/net/nf_conntrack'

# The same connection, but with the guard's conntrack rule removed.
ct_handle=$(nft -a list chain inet odac_guard forward_guard \
    | sed -n '/ct state/{s/.*# handle \([0-9][0-9]*\).*/\1/p;}')
nft delete rule inet odac_guard forward_guard handle "$ct_handle"
ip netns exec client curl -s --noproxy '*' --max-time 3 -o /dev/null \
    "http://198.51.100.1:9/" >/dev/null 2>&1
rc2=$?
echo "# without the rule, the exit code: $rc2"
echo "# the conntrack states present: $(ct_states)"

if [ "$rc" = "$rc2" ]; then
    ok 'the conntrack rule does not change the outcome of a new connection'
else
    bad 'the conntrack rule changes the outcome of a new connection'
fi

echo "# pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
