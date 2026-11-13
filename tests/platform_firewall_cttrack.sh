#!/bin/sh
# Is conntrack tracking TCP in this namespace?
#
# A closed port answers with a RST, and a RST makes the flow invalid, so the
# conntrack entry disappears again. A listener that stays open keeps the
# entry, and then the table shows the flow and its state.
set -u
[ "$(id -u)" -eq 0 ] || { echo 'run as root inside unshare -mn' >&2; exit 2; }
mount -t tmpfs tmpfs /run; mkdir -p /run/netns
mount -t sysfs sysfs /sys
ip netns add client; ip netns add inet
ip link add rtr0 type veth peer name c0 netns client
ip link add up0 type veth peer name i0 netns inet
ip link set lo up; ip link set rtr0 up; ip link set up0 up
ip addr add 10.250.5.1/24 dev rtr0; ip addr add 198.51.100.254/24 dev up0
ip netns exec client sh -c "ip link set lo up; ip link set c0 up; ip addr add 10.250.5.2/24 dev c0; ip route add default via 10.250.5.1"
ip netns exec inet sh -c "ip link set lo up; ip link set i0 up; ip addr add 198.51.100.1/24 dev i0; ip route add default via 198.51.100.254"
ip route add default via 198.51.100.1
echo 1 > /proc/sys/net/ipv4/ip_forward

echo "=== the conntrack settings that decide this ==="
for k in nf_conntrack_tcp_loose nf_conntrack_acct nf_conntrack_checksum \
         nf_conntrack_events nf_conntrack_helper; do
    printf '  %-28s %s\n' "$k" "$(cat /proc/sys/net/netfilter/$k 2>&1)"
done

echo
echo "=== a connection to a closed port (the RST case) ==="
ip netns exec client curl -s --noproxy '*' --max-time 4 -o /dev/null \
    "http://198.51.100.1:9/" >/dev/null 2>&1
echo "  the exit code: $?"
echo "  the count: $(cat /proc/sys/net/netfilter/nf_conntrack_count 2>&1)"
cat /proc/net/nf_conntrack 2>/dev/null \
    | sed 's/[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}/A/g' | sed 's/^/    /'

echo
echo "=== a connection to a port that stays open ==="
# A listener in the internet namespace that holds the connection open.
ip netns exec inet sh -c 'nc -l -p 8080 >/dev/null 2>&1' &
sleep 1
ip netns exec client curl -s --noproxy '*' --max-time 3 -o /dev/null \
    "http://198.51.100.1:8080/" >/dev/null 2>&1
echo "  the exit code: $?"
echo "  the count: $(cat /proc/sys/net/netfilter/nf_conntrack_count 2>&1)"
cat /proc/net/nf_conntrack 2>/dev/null \
    | sed 's/[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}/A/g' | sed 's/^/    /'

echo
echo "=== an ICMP echo, for comparison ==="
ip netns exec client ping -n -c1 -W1 198.51.100.1 >/dev/null 2>&1
echo "  the count: $(cat /proc/sys/net/netfilter/nf_conntrack_count 2>&1)"
cat /proc/net/nf_conntrack 2>/dev/null \
    | sed 's/[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}\.[0-9]\{1,3\}/A/g' | sed 's/^/    /'
