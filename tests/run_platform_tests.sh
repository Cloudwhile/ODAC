#!/bin/sh
# Runs platform tests on a Linux host without touching host networking.
# Requires: cc, json-c headers, iproute2, nftables, unshare
# (unprivileged user namespaces). Everything runs inside unshare -rnm.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$root"/tests/fixtures/platform/ndsctl "$root"/tests/fixtures/platform/ip "$root"/tests/fixtures/platform/odac-firewall "$work"/
chmod +x "$work"/ndsctl "$work"/ip "$work"/odac-firewall
: > "$work/log"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wpedantic -Wconversion -Wno-sign-conversion -O2 -D_FORTIFY_SOURCE=2 \
    ${SANITIZE--fsanitize=address,undefined} -I "$root/device-access/src" \
    -DODAC_STATUS_OWNER=0 -DODAC_NDSCTL="\"$work/ndsctl\"" -DODAC_IP="\"$work/ip\"" \
    -DODAC_FIREWALL="\"$work/odac-firewall\"" -DODAC_CONNTRACK='"/bin/true"' -DODAC_UBUS='"/bin/false"' \
    -DODAC_DHCP_LEASES='"/nonexistent"' \
    "$root/tests/platform_test.c" "$root"/device-access/src/platform*.c -ljson-c -o "$work/platform_test"
ODAC_TEST_LOG="$work/log" unshare -rnm sh -c 'mount -t sysfs sysfs /sys && ip link add odactest0 type dummy && exec "$1"' sh "$work/platform_test"
# Run the netns test from $work too: when started as root, unshare -r maps only
# uid 0, so files owned by another uid (a CI checkout) cannot be opened there.
mkdir -p "$work/netns/tests" "$work/netns/device-access/files/usr/libexec"
cp "$root/tests/platform_firewall_netns.sh" "$work/netns/tests/"
cp "$root/tests/platform_firewall_egress.sh" "$work/netns/tests/"
cp "$root/device-access/files/usr/libexec/odac-firewall" "$work/netns/device-access/files/usr/libexec/"
chmod +x "$work/netns/device-access/files/usr/libexec/odac-firewall"
timeout 180 unshare -rnm sh "$work/netns/tests/platform_firewall_netns.sh"
timeout 180 unshare -rnm sh "$work/netns/tests/platform_firewall_egress.sh"
