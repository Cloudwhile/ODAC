#!/bin/sh
# Build the four ODAC packages inside an extracted OpenWrt SDK.
#
# Usage: build-sdk.sh SDK_DIR TARGET SUBTARGET
#
# Evidence behind the choices in this script
#
#   * The feeds interface takes "-p <feed> <package>" or a bare package name.
#     "./scripts/feeds install base/libjson-c" looks for a package literally
#     named "base/libjson-c", finds none and only prints
#     "WARNING: No feed for package 'base/libjson-c' found" while still
#     exiting 0. The precise selection therefore has to use the documented
#     syntax, and the result has to be verified afterwards.
#
#   * install_src() walks source and host build dependencies, including other
#     variants in a source recipe. Feed installation alone does not establish
#     which variant causes compilation. CI shows GnuTLS using host gettext's
#     autopoint; the exact generated dependency edges and repeated GnuTLS
#     build still need tracing (docs/root-cause.md). Keep dependencies intact.
#
#   * The SDK ships no .config. A minimal configuration is written first and
#     then completed by "make defconfig", which is the only way to get a
#     consistent selection.
#
#   * "make defconfig" writes a disabled boolean as "# CONFIG_X is not set",
#     not as "CONFIG_X=n". The verification therefore accepts the comment
#     form. CONFIG_ALL must be explicitly disabled in the input as well:
#     omitting it would restore the SDK's default y.
set -eu

[ "$#" -eq 3 ] || { echo 'Usage: build-sdk.sh SDK_DIR TARGET SUBTARGET' >&2; exit 2; }
sdk=$(realpath "$1")
target=$2
subtarget=$3
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ "$(uname -s)" = Linux ] || { echo 'The official SDK requires Linux' >&2; exit 1; }
[ -f "$sdk/include/package-pack.mk" ] && [ -x "$sdk/scripts/feeds" ] ||
	{ echo 'Not an extracted OpenWrt SDK' >&2; exit 1; }
[ ! -e "$sdk/package/odac" ] || { echo 'SDK package/odac already exists; review before replacement' >&2; exit 1; }

# The SDK release is identified from its own version metadata. The SDK ships
# no .config, so include/version.mk is the authoritative source: its
# VERSION_NUMBER falls back to the release the SDK was cut from.
version=$(sed -n 's/^VERSION_NUMBER:=$(if $(VERSION_NUMBER),$(VERSION_NUMBER),\([^)]*\))$/\1/p' "$sdk/include/version.mk")
printf 'SDK release: %s\n' "${version:-unknown}"
case "$version" in
	25.12.*) ;;
	*) echo "SDK must identify OpenWrt 25.12.x, found '${version:-nothing}'" >&2; exit 1;;
esac

# The SDK is target specific and ships no .config, so the archive name is the
# only evidence of what it actually builds. Building an x86/64 SDK and
# renaming the output would be a forged target, so the name is checked
# against the requested target instead of being trusted.
sdk_name=$(basename "$sdk")
case "$sdk_name" in
	"openwrt-sdk-${version}-${target}-${subtarget}_"*) :;;
	*)
		echo "SDK directory '${sdk_name}' does not match the requested target ${target}/${subtarget}" >&2
		exit 1;;
esac

mkdir "$sdk/package/odac"
for package in argon2 odac-opennds device-access luci-app-device-access; do
	cp -R "$project/$package" "$sdk/package/odac/"
done
cd "$sdk"

# Record elapsed time and the original exit status, including failed stages.
# Compile targets also package their output; these are combined timings.
stage_name=''
stage_start=0
record_stage() {
	[ -n "$stage_name" ] || return 0
	if [ -n "${ODAC_BUILD_METRICS:-}" ]; then
		printf '%s\t%s\t%s\n' "$stage_name" "$(($(date +%s) - stage_start))" "$1" >> "$ODAC_BUILD_METRICS"
	fi
}
start_stage() {
	record_stage 0
	stage_name=$1
	stage_start=$(date +%s)
}
trap 'rc=$?; trap - EXIT; record_stage "$rc"; exit "$rc"' EXIT
start_stage feeds

# --- feeds -------------------------------------------------------------------
# The feed revisions are the ones the released SDK was built from. They are
# read from feeds.conf.default and are not rewritten, so the build cannot
# silently mix in a newer feed.

printf '\n==> feeds update\n'
for feed in base packages luci; do
	./scripts/feeds update "$feed"
done

printf '\n==> feeds install (precise selection)\n'
# The feed and the package name are separate arguments. A wrong package name
# only produces a warning, so the installed set is verified below.
install_feed_package() {
	feed=$1
	package=$2
	./scripts/feeds install -p "$feed" "$package"
}

# Runtime dependencies of the three ODAC packages, by owning feed:
#   base:    libjson-c ca-bundle uhttpd firewall4 nftables-json rpcd uci
#            ubus jshn jsonfilter openssl-util
#   packages: libsqlite3 curl conntrack libmicrohttpd-no-ssl
#   luci:    luci-base rpcd-mod-luci
#
# The stock routing/opennds recipe is not used: the local odac-opennds
# recipe builds the same pinned source with a gated init script, so the
# routing feed is neither updated nor installed.
#
# No libustream variant is installed on purpose. The three variants are
# mutually exclusive and all install /lib/libustream-ssl.so, so selecting
# one here would decide the TLS backend of the produced image. device-access
# itself does not link libustream, and uhttpd loads the provider at runtime
# from whatever the base system already provides.
install_feed_package base libjson-c
install_feed_package base ca-bundle
install_feed_package base uhttpd
install_feed_package base firewall4
install_feed_package base nftables-json
install_feed_package base rpcd
install_feed_package base uci
install_feed_package base ubus
install_feed_package base jshn
install_feed_package base jsonfilter
install_feed_package base openssl-util
install_feed_package packages libsqlite3
install_feed_package packages curl
install_feed_package packages conntrack
install_feed_package packages libmicrohttpd-no-ssl
install_feed_package luci luci-base
install_feed_package luci rpcd-mod-luci

# --- configuration -----------------------------------------------------------

start_stage configuration
printf '\n==> configuring\n'
# The SDK ships no .config. A minimal, explicit configuration is written and
# then completed by defconfig, which resolves the dependencies.
{
	echo "CONFIG_TARGET_${target}=y"
	echo "CONFIG_TARGET_${target}_${subtarget}=y"
	echo "CONFIG_TARGET_BOARD=\"${target}\""
	echo "CONFIG_TARGET_SUBTARGET=\"${subtarget}\""
	echo "CONFIG_USE_APK=y"
	printf '# CONFIG_ALL is not set\n# CONFIG_ALL_NONSHARED is not set\n# CONFIG_ALL_KMODS is not set\n'
	for package in libargon2 odac-opennds device-access luci-app-device-access; do
		printf 'CONFIG_PACKAGE_%s=m\n' "$package"
	done
} > .config

# An absent CONFIG_ALL takes the SDK default y. Explicitly disable it and
# select the four roots above; defconfig resolves their dependency closure.

make defconfig

# --- verification ------------------------------------------------------------

# The selected symbols are read from the generated .config. A disabled
# boolean is written as a comment, so both forms are checked.
symbol_value() {
	# Prints "y", "m" or "n" for a config symbol.
	symbol=$1
	if grep -Fqx "CONFIG_${symbol}=y" .config; then
		echo y
	elif grep -Fqx "CONFIG_${symbol}=m" .config; then
		echo m
	elif grep -Fqx "# CONFIG_${symbol} is not set" .config; then
		echo n
	else
		echo absent
	fi
}

require_enabled() {
	symbol=$1
	value=$(symbol_value "$symbol")
	case "$value" in
		y|m) :;;
		*) echo "Required symbol CONFIG_${symbol} is '${value}'" >&2; exit 1;;
	esac
}

# The four ODAC packages must be selected.
for package in libargon2 odac-opennds device-access luci-app-device-access; do
	require_enabled "PACKAGE_${package}"
done

# CONFIG_ALL must be off, otherwise the build compiles every package of
# every feed again.
if [ "$(symbol_value ALL)" != n ]; then
	echo 'CONFIG_ALL is not disabled; the build scope would widen' >&2
	exit 1
fi

# The runtime dependencies of the ODAC packages must be selected, so that the
# produced APKs carry a complete dependency closure.
for symbol in \
	PACKAGE_libsqlite3 PACKAGE_libjson-c PACKAGE_libargon2 PACKAGE_ca-bundle \
	PACKAGE_uhttpd PACKAGE_libmicrohttpd-no-ssl PACKAGE_firewall4 PACKAGE_nftables-json \
	PACKAGE_conntrack PACKAGE_rpcd PACKAGE_uci PACKAGE_ubus PACKAGE_jshn \
	PACKAGE_jsonfilter PACKAGE_curl PACKAGE_openssl-util \
	PACKAGE_luci-base PACKAGE_rpcd-mod-luci
do
	require_enabled "$symbol"
done

# The stock opennds package must not be selected: it conflicts with
# odac-opennds and its postinst start changes network.wan6.
if [ "$(symbol_value PACKAGE_opennds)" = y ] || [ "$(symbol_value PACKAGE_opennds)" = m ]; then
	echo 'The stock opennds package is selected; it must not be built with ODAC' >&2
	exit 1
fi

printf '\nSelected packages: %s\n' "$(grep -cE '^CONFIG_PACKAGE_.*=[ym]$' .config)"
printf '\n>>> The packages this build compiles:\n'
grep -E '^CONFIG_PACKAGE_.*=[ym]$' .config | sed 's/^/    /'

# --- build -------------------------------------------------------------------

printf '\n==> building\n'
start_stage argon2_compile_and_package
make package/odac/argon2/compile V=s
start_stage admission_compile_and_package
make package/odac/odac-opennds/compile V=s
start_stage backend_compile_and_package
make package/odac/device-access/compile V=s
start_stage luci_compile_and_package
make package/odac/luci-app-device-access/compile V=s
start_stage package_discovery

printf '\n==> produced packages\n'
# FeedPackageDir may use bin/packages/<arch>/<feed> or the target's
# bin/targets/<board>/<subtarget>/packages directory. Search both beneath
# this fresh SDK's bin tree; collection validates package metadata separately.
package_dir="bin"
[ -d "$package_dir" ] || { echo "the package directory ${package_dir} does not exist" >&2; exit 1; }
find "$package_dir" -type f \( -name 'libargon2-*.apk' -o -name 'odac-opennds-*.apk' -o -name 'device-access-*.apk' -o -name 'luci-app-device-access-*.apk' \) -print
