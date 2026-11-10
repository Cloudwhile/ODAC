#!/bin/sh
# Run locally on a staging router; read-only and credential-safe by construction.
# No SSH, package installs, probes, logs, process argv, or complete UCI dumps.
set -u
printf '%s\n' 'ODAC environment report (no configuration secrets collected)'
if [ -r /etc/openwrt_release ]; then
 sed -n '/^DISTRIB_RELEASE=/p; /^DISTRIB_TARGET=/p; /^DISTRIB_ARCH=/p' /etc/openwrt_release
else
 echo 'Not an OpenWrt runtime'; exit 1
fi
uname -r
for package in opennds uhttpd rpcd firewall4 nftables-json conntrack libsqlite3 libjson-c libargon2 libustream-mbedtls openssl-util; do
 printf '\nPackage: %s\n' "$package"
 apk info -v "$package" 2>/dev/null || echo 'not installed'
done
printf '\nNative API help (no runtime client records):\n'
ndsctl -h 2>&1 || true
nft --version 2>&1 || true
fw4 -h 2>&1 || true
printf '\nResource totals (not process command lines):\n'
sed -n '/^MemTotal:/p; /^MemAvailable:/p' /proc/meminfo
df -k /overlay /tmp 2>/dev/null || true
printf '\nODAC nonsecret selection and acceleration flags:\n'
for key in network interface zone gateway portal_port portal_https_port portal_hostname enabled; do
 printf '%s=' "$key"; uci -q get "odac.main.$key" || true
done
for key in flow_offloading flow_offloading_hw; do
 printf '%s=' "$key"; uci -q get "firewall.@defaults[0].$key" || true
done
printf '\nDo not append full UCI, logs, environment, ndsctl JSON, or upstream script content.\n'
