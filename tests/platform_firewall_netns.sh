#!/bin/sh
# Real kernel nftables test for odac-firewall. Never touches host networking:
# run as   unshare -rnm sh tests/platform_firewall_netns.sh
# It creates an unprivileged user+network+mount namespace with nested netns.
# Topology: client(10.250.5.2, c0) -- router(odactest0 / up0) -- internet(198.51.100.1)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
fw="$here/../device-access/files/usr/libexec/odac-firewall"
[ "$(id -u)" -eq 0 ] || { echo 'run inside unshare -rnm' >&2; exit 2; }
mount -t tmpfs tmpfs /run; mkdir -p /run/netns
mount -t sysfs sysfs /sys
ip netns add client; ip netns add inet
# IPv6 ULA test prefixes: client side :5::/64, upstream side :6::/64
for n in '' 'ip netns exec client' 'ip netns exec inet'; do $n sh -c 'echo 0 > /proc/sys/net/ipv6/conf/default/accept_dad; echo 0 > /proc/sys/net/ipv6/conf/all/accept_dad'; done
net_c=$(printf 'fd00:%x::' 5); net_u=$(printf 'fd00:%x::' 6)
ip link add odactest0 type veth peer name c0 netns client
ip link add up0 type veth peer name i0 netns inet
ip link set lo up; ip link set odactest0 up; ip link set up0 up
ip addr add 10.250.5.1/24 dev odactest0; ip addr add 198.51.100.254/24 dev up0
# Upstream portal addresses and a private "management" address live on the WAN side.
ip addr add 10.83.77.1/24 dev up0; ip addr add 10.95.49.1/24 dev up0
ip netns exec client sh -c "ip link set lo up; ip link set c0 address aa:bb:cc:dd:ee:01; ip link set c0 up; ip addr add 10.250.5.2/24 dev c0; ip route add default via 10.250.5.1; ip -6 addr add ${net_c}8/64 dev c0 nodad; ip -6 route add default via ${net_c}1"
ip -6 addr add ${net_c}1/64 dev odactest0 nodad; ip -6 addr add ${net_u}1/64 dev up0 nodad
ip route add default via 198.51.100.1
ip netns exec inet sh -c "ip link set lo up; ip link set i0 up; ip addr add 198.51.100.1/24 dev i0; ip addr add 203.0.113.10/24 dev i0; ip addr add 203.0.113.20/24 dev i0; ip addr add 10.14.55.9/24 dev i0; ip route add default via 198.51.100.254; ip -6 addr add ${net_u}2/64 dev i0 nodad; ip -6 route add default via ${net_u}1"
echo 1 > /proc/sys/net/ipv4/ip_forward
echo 1 > /proc/sys/net/ipv6/conf/all/forwarding
# A permissive "fw4" stand-in at priority 0 accepts everything: proves our drops win.
nft -f - <<'EOF'
table inet fw4 {
    chain forward { type filter hook forward priority 0; policy accept; ct state established,related accept; accept; }
    chain input { type filter hook input priority 0; policy accept; }
}
EOF
pass=0; fail=0
ok() { pass=$((pass+1)); echo "ok - $1"; }
bad() { fail=$((fail+1)); echo "not ok - $1"; }
ping4() { ip netns exec client ping -n -c1 -W1 "$1" >/dev/null 2>&1; }
ping6() { ip netns exec client ping -6 -n -c1 -W1 "$1" >/dev/null 2>&1; }
expect() { if "$@" >/dev/null 2>&1; then ok "$label"; else bad "$label"; fi; }
expect_not() { if "$@" >/dev/null 2>&1; then bad "$label"; else ok "$label"; fi; }
ip netns exec client ping -c1 -W1 10.250.5.1 >/dev/null 2>&1 || true   # populate neighbor
label='no guard: baseline forwarding works'; expect ping4 198.51.100.1
label='no guard: baseline private forwarding works'; expect ping4 10.14.55.9
label='no guard: baseline upstream portal reachable'; expect ping4 203.0.113.10
label='no guard: baseline IPv6 forwarding works'; expect ping6 "${net_u}2"

label='rejects injected interface'; expect_not "$fw" init 'odactest0;x' 10.250.5.1 2080 8443
label='rejects loopback'; expect_not "$fw" init lo 10.250.5.1 2080 8443
label='rejects bad gateway'; expect_not "$fw" init odactest0 '10.250.5.1 }' 2080 8443
label='rejects zero port'; expect_not "$fw" init odactest0 10.250.5.1 0 8443
label='rejects ssh port'; expect_not "$fw" init odactest0 10.250.5.1 22 8443
label='rejects LuCI https port'; expect_not "$fw" init odactest0 10.250.5.1 2080 443
label='rejects LuCI http port'; expect_not "$fw" init odactest0 10.250.5.1 80 8443
label='rejects equal ports'; expect_not "$fw" init odactest0 10.250.5.1 8443 8443
label='rejects bad upstream address'; expect_not "$fw" init odactest0 10.250.5.1 2080 8443 '203.0.113.10 }'
label='rejects more than 16 upstream addresses'; expect_not "$fw" init odactest0 10.250.5.1 2080 8443 $(i=1; while [ $i -le 17 ]; do printf '203.0.113.%s ' $i; i=$((i+1)); done)
if ip link add odactun0 type ipip local 10.250.5.1 remote 10.226.2.6 2>/dev/null; then
    label='rejects non-Ethernet interface'; expect_not "$fw" init odactun0 10.250.5.1 2080 8443
else echo 'skip - non-Ethernet interface (ipip unavailable)'; fi
label='rejects op before init'; expect_not "$fw" gate odactest0 online 60
label='init without upstream addresses'; expect "$fw" init odactest0 10.250.5.1 2080 8443
"$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2 60; "$fw" gate odactest0 online 60
label='no upstream configured: portal address not blocked'; expect ping4 203.0.113.10
label='init'; expect "$fw" init odactest0 10.250.5.1 2080 8443 203.0.113.10 203.0.113.20
label='init is idempotent'; expect "$fw" init odactest0 10.250.5.1 2080 8443 203.0.113.10 203.0.113.20
label='exact interface: prefix rejected'; expect_not "$fw" gate odactest online 60
label='other existing iface rejected'; expect_not "$fw" gate up0 online 60
label='closed: client blocked'; expect_not ping4 198.51.100.1
label='closed: IPv6 blocked'; expect_not ping6 "${net_u}2"
label='closed: router ICMP input blocked'; expect_not ping4 10.250.5.1
label='closed: router itself still reaches upstream portal'; expect ping -n -c1 -W1 203.0.113.10
label='closed: router reaches internet (output unaffected)'; expect ping -n -c1 -W1 198.51.100.1

"$fw" gate odactest0 online 60
label='gate only: unauthorized client still blocked'; expect_not ping4 198.51.100.1
label='rejects multicast MAC'; expect_not "$fw" auth odactest0 01:00:5e:00:00:01 10.250.5.2 60
label='rejects lease >90'; expect_not "$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2 91
label='rejects leading-zero lease'; expect_not "$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2 060
label='rejects injected ip'; expect_not "$fw" auth odactest0 aa:bb:cc:dd:ee:01 '10.250.5.2 }; flush ruleset; {' 60
label='rejects octet >255'; expect_not "$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.256 60
label='auth'; expect "$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2 60
label='auth + gate: client reaches internet'; expect ping4 198.51.100.1
label='auth: IPv6 still blocked'; expect_not ping6 "${net_u}2"
label='auth: first configured upstream address blocked'; expect_not ping4 203.0.113.10
label='auth: second configured upstream address blocked'; expect_not ping4 203.0.113.20
label='auth: router private/management address blocked'; expect_not ping4 10.83.77.1
label='auth: forwarded private range blocked'; expect_not ping4 10.14.55.9
label='auth refresh idempotent'; expect "$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2 60
ip netns exec client ip addr add 10.199.9.8/24 dev c0
label='spoofed second IP same MAC blocked'; expect_not ip netns exec client ping -n -c1 -W1 -I 10.199.9.8 198.51.100.1
label='WAN closed: authorized client blocked'; "$fw" gate odactest0 closed 1; expect_not ping4 198.51.100.1
"$fw" gate odactest0 online 60
label='WAN reopened: authorized client restored without reauth'; expect ping4 198.51.100.1
label='deauth'; expect "$fw" deauth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2
label='deauth revokes'; expect_not ping4 198.51.100.1
label='deauth absent element is ok'; expect "$fw" deauth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2
"$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2 2
label='short lease works'; expect ping4 198.51.100.1
sleep 3   # Time-under-test: nft element expiry itself.
label='lease expires without daemon (crash safety)'; expect_not ping4 198.51.100.1
"$fw" auth odactest0 aa:bb:cc:dd:ee:01 10.250.5.2 60
"$fw" gate odactest0 online 2
sleep 3   # Time-under-test: WAN gate expiry.
label='gate expires without daemon'; expect_not ping4 198.51.100.1
"$fw" gate odactest0 online 60
label='close'; expect "$fw" close odactest0
label='close revokes'; expect_not ping4 198.51.100.1
label='table persists after close'; expect nft list table inet odac_guard
# fw4 reload replaces only table inet fw4.
nft -f - <<'EOF'
delete table inet fw4
table inet fw4 {
    chain forward { type filter hook forward priority 0; policy accept; accept; }
}
EOF
label='guard survives fw4 table reload'; expect nft list chain inet odac_guard forward_guard
label='after reload: still blocked'; expect_not ping4 198.51.100.1
"$fw" init odactest0 10.250.5.1 2080 8443 >/dev/null
label='re-init closes everything'; expect_not ping4 198.51.100.1
echo "# pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
