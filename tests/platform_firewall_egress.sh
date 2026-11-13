#!/bin/sh
# The guard is not the only thing that decides whether a controlled client
# reaches the internet.
#
# odac-firewall runs in the forward hook at priority -10 and only drops; it
# never accepts. fw4's forward chain runs at priority 0, so fw4 decides what
# the guard let through. A controlled zone whose forward policy is REJECT and
# that has no forwarding section to the uplink zone is therefore rejected by
# fw4 even for a client the guard has admitted.
#
# fw4 25.12 renders, for a zone with the forward policy REJECT:
#
#   chain forward          { iifname "<dev>" jump forward_<zone> ... jump handle_reject }
#   chain forward_<zone>   { jump reject_to_<zone> }
#   chain reject_to_<zone> { oifname "<dev>" jump handle_reject }
#   chain handle_reject    { meta l4proto tcp reject with tcp reset; reject }
#
# A forwarding section from the controlled zone to the uplink zone adds a
# jump to accept_to_<uplink> in front of the policy jump, and the uplink
# zone's masquerade adds a srcnat chain with the masquerade rule.
#
# Every verdict is observed by the kernel. The far end counts the SYNs that
# reach it in its own nft counter, so "the guard let it through" and "fw4
# rejected it" are told apart by whether the SYN arrived. The client's own
# curl exit code says what happened to the client:
#
#   0   the HTTP transaction completed
#   7   the connection was refused (the router sent a TCP reset)
#   28  the SYN was silently dropped, or the reply never came back
#
# The far end has no route to the controlled network, so only a masqueraded
# source address gives it a way back to the client. That is what makes the
# masquerade observable.
#
# Run as   unshare -rnm sh tests/platform_firewall_egress.sh
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
# The far end has no route to the controlled network on purpose: only a
# masqueraded source address gives it a way back to the client.
#
# Reverse path filtering is turned off in the far end's namespace: with it on,
# the kernel there would drop an unmasquerized SYN as arriving on the wrong
# interface, and the test would then measure the far end's filtering instead
# of the router's.
ip netns exec inet sh -c "ip link set lo up; ip link set i0 up; ip addr add 198.51.100.1/24 dev i0; echo 0 > /proc/sys/net/ipv4/conf/all/rp_filter; echo 0 > /proc/sys/net/ipv4/conf/i0/rp_filter"
ip route add default via 198.51.100.1
echo 1 > /proc/sys/net/ipv4/ip_forward
pass=0; fail=0
ok() { pass=$((pass+1)); echo "ok - $1"; }
bad() { fail=$((fail+1)); echo "not ok - $1"; }
check() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (got '$2', expected '$3')"; fi; }
yesno() { if [ "$1" -gt "$2" ]; then echo yes; else echo no; fi; }

# The far end counts every SYN that reaches it, in its own namespace.
ip netns exec inet nft -f - >/dev/null 2>&1 <<'EOF'
table inet probe {
    chain probein { type filter hook input priority -100; policy accept; }
}
EOF
ip netns exec inet nft add rule inet probe probein tcp dport 8080 counter >/dev/null 2>&1
far_end_syns() {
    ip netns exec inet nft list chain inet probe probein 2>/dev/null \
        | sed -n 's/.*counter packets \([0-9]*\) bytes.*/\1/p' | head -1
}

# The far end answers the request, so a completed transaction means the
# client's SYN arrived and the reply found its way back.
ip netns exec inet python3 -m http.server 8080 --bind 198.51.100.1 >/dev/null 2>&1 &
far_pid=$!
sleep 2

client_curl() {
    ip netns exec client curl -s --noproxy '*' --max-time 5 -o /dev/null \
        "http://198.51.100.1:8080/" >/dev/null 2>&1
    echo "$?"
}

# --- fw4's chains, built the way fw4 builds them ---------------------------
#   FORWARDING  1 when a forwarding section to the uplink zone exists
#   MASQ        1 when the uplink zone masquerades
build_fw4() {
    nft delete table inet fw4 2>/dev/null
    {
        echo 'table inet fw4 {'
        echo '    chain forward {'
        echo '        type filter hook forward priority filter; policy drop;'
        echo '        ct state vmap { established : accept, related : accept }'
        echo '        iifname "odactest0" jump forward_odac'
        echo '        iifname "up0" jump forward_uplink'
        echo '        jump handle_reject'
        echo '    }'
        echo '    chain forward_odac {'
        [ "$FORWARDING" = 1 ] && echo '        jump accept_to_uplink'
        echo '        jump reject_to_odac'
        echo '    }'
        echo '    chain forward_uplink {'
        echo '        jump accept_to_uplink'
        echo '    }'
        echo '    chain accept_to_uplink {'
        echo '        oifname "up0" accept'
        echo '    }'
        echo '    chain reject_to_odac {'
        echo '        oifname "odactest0" jump handle_reject'
        echo '    }'
        echo '    chain handle_reject {'
        echo '        meta l4proto tcp reject with tcp reset'
        echo '        reject'
        echo '    }'
        echo '    chain srcnat {'
        echo '        type nat hook postrouting priority srcnat; policy accept;'
        [ "$MASQ" = 1 ] && echo '        oifname "up0" jump srcnat_uplink'
        echo '    }'
        echo '    chain srcnat_uplink {'
        echo '        meta nfproto ipv4 masquerade'
        echo '    }'
        echo '}'
    } > /run/fw4.nft
    if ! nft -f /run/fw4.nft >/dev/null 2>/tmp/fw4.err; then
        echo 'fw4 chains were not installed' >&2
        sed 's/^/  /' /tmp/fw4.err >&2
        exit 1
    fi
}

# --- the guard --------------------------------------------------------------
# AUTHORISED=1 admits the client; AUTHORISED=0 leaves it unknown, so the
# guard drops it.
build_guard() {
    nft delete table inet odac_guard 2>/dev/null
    "$fw" init odactest0 10.250.5.1 2080 8443
    if [ "$AUTHORISED" = 1 ]; then
        "$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2 60
        "$fw" gate odactest0 online 60
    fi
}

echo "=== the zone as it is configured on the device: no forwarding section ==="
FORWARDING=0 MASQ=0 build_fw4
AUTHORISED=1 build_guard
before=$(far_end_syns)
rc=$(client_curl)
after=$(far_end_syns)
check 'the guard let the client through, so fw4 refused the connection' "$rc" 7
check 'the SYN never reached the far end' "$after" "$before"

echo
echo "=== the guard drops an unauthorised client ==="
FORWARDING=0 MASQ=0 build_fw4
AUTHORISED=0 build_guard
before=$(far_end_syns)
rc=$(client_curl)
after=$(far_end_syns)
check 'the guard dropped it silently' "$rc" 28
check 'the SYN never reached the far end' "$after" "$before"

echo
echo "=== with a forwarding section, but without masquerading ==="
FORWARDING=1 MASQ=0 build_fw4
AUTHORISED=1 build_guard
before=$(far_end_syns)
rc=$(client_curl)
after=$(far_end_syns)
check 'the SYN reached the far end' "$(yesno "$after" "$before")" yes
check 'but the reply had no way back, so the client timed out' "$rc" 28

echo
echo "=== with a forwarding section and masquerading ==="
FORWARDING=1 MASQ=1 build_fw4
AUTHORISED=1 build_guard
before=$(far_end_syns)
rc=$(client_curl)
after=$(far_end_syns)
check 'the SYN reached the far end' "$(yesno "$after" "$before")" yes
check 'and the client got its answer' "$rc" 0

echo
echo "=== with a forwarding section, the guard still drops an unauthorised client ==="
FORWARDING=1 MASQ=1 build_fw4
AUTHORISED=0 build_guard
before=$(far_end_syns)
rc=$(client_curl)
after=$(far_end_syns)
check 'the guard dropped it silently' "$rc" 28
check 'the SYN never reached the far end' "$after" "$before"

kill "$far_pid" 2>/dev/null
wait "$far_pid" 2>/dev/null
echo "# pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
