#!/bin/sh
# Exercise the SDK build entry point without downloading feeds or building.
#
# The stub "feeds" and "make" record what the build script asked for, so the
# assertions check the real interface usage rather than a description of it.
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
sdk="$work/openwrt-sdk-25.12.2-x86-64_gcc-14.3.0_musl.Linux-x86_64"
mkdir -p "$sdk/include" "$sdk/scripts" "$sdk/package"
: > "$sdk/include/package-pack.mk"
printf 'VERSION_NUMBER:=$(if $(VERSION_NUMBER),$(VERSION_NUMBER),25.12.2)\n' > "$sdk/include/version.mk"

PASS=0
FAILURES=0
ok() { PASS=$((PASS + 1)); printf 'PASS: %s\n' "$1"; }
not_ok() { FAILURES=$((FAILURES + 1)); printf 'FAIL: %s\n' "$1" >&2; }

# The stub feeds script exits 42 on the first install, so the test can see
# that the script reached the feeds phase at all.
cat > "$sdk/scripts/feeds" <<'EOF'
#!/bin/sh
exit 42
EOF
chmod +x "$sdk/scripts/feeds"

# run_build TARGET SUBTARGET [EXTRA_ENV...]
run_build() {
	target=$1
	subtarget=$2
	shift 2
	rm -rf "$sdk/package/odac"
	rc=0
	env "$@" sh "$project/scripts/build-sdk.sh" "$sdk" "$target" "$subtarget" \
		> "$work/output" 2>&1 || rc=$?
	return $rc
}

# --- the SDK release and target are verified ---------------------------------

rc=0
run_build x86 64 || rc=$?
if [ "$rc" -eq 42 ]; then
	ok 'the build reaches the feeds phase on a matching SDK'
else
	not_ok "the build reached the feeds phase (exit $rc)"
	cat "$work/output" >&2
fi

# An SDK directory that does not match the requested target is a forged
# target and must be rejected before anything is built.
mkdir -p "$work/openwrt-sdk-25.12.2-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64/include" \
	"$work/openwrt-sdk-25.12.2-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64/scripts"
printf 'VERSION_NUMBER:=$(if $(VERSION_NUMBER),$(VERSION_NUMBER),25.12.2)\n' \
	> "$work/openwrt-sdk-25.12.2-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64/include/version.mk"
: > "$work/openwrt-sdk-25.12.2-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64/include/package-pack.mk"
cp "$sdk/scripts/feeds" "$work/openwrt-sdk-25.12.2-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64/scripts/feeds"
rc=0
sh "$project/scripts/build-sdk.sh" \
	"$work/openwrt-sdk-25.12.2-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64" x86 64 \
	> "$work/output" 2>&1 || rc=$?
if [ "$rc" -ne 0 ]; then
	ok 'an SDK that does not match the requested target is rejected'
else
	not_ok 'an SDK that does not match the requested target is rejected'
fi

# An SDK of a different release is rejected.
mkdir -p "$work/openwrt-sdk-24.10.0-x86-64_gcc-13.3.0_musl.Linux-x86_64/include" \
	"$work/openwrt-sdk-24.10.0-x86-64_gcc-13.3.0_musl.Linux-x86_64/scripts"
printf 'VERSION_NUMBER:=$(if $(VERSION_NUMBER),$(VERSION_NUMBER),24.10.0)\n' \
	> "$work/openwrt-sdk-24.10.0-x86-64_gcc-13.3.0_musl.Linux-x86_64/include/version.mk"
: > "$work/openwrt-sdk-24.10.0-x86-64_gcc-13.3.0_musl.Linux-x86_64/include/package-pack.mk"
cp "$sdk/scripts/feeds" "$work/openwrt-sdk-24.10.0-x86-64_gcc-13.3.0_musl.Linux-x86_64/scripts/feeds"
rc=0
sh "$project/scripts/build-sdk.sh" \
	"$work/openwrt-sdk-24.10.0-x86-64_gcc-13.3.0_musl.Linux-x86_64" x86 64 \
	> "$work/output" 2>&1 || rc=$?
if [ "$rc" -ne 0 ]; then
	ok 'an SDK of an unsupported release is rejected'
else
	not_ok 'an SDK of an unsupported release is rejected'
fi

# --- the feeds selection is precise and uses the real interface ---------------

cat > "$sdk/scripts/feeds" <<'EOF'
#!/bin/sh
# Record every feed operation so the test can inspect them.
case "$1" in
	update) echo "update $2" >> "${FEEDS_LOG}" ;;
	install)
		shift
		case "$1" in
			-p) echo "install -p $2 $3" >> "${FEEDS_LOG}";;
			*) echo "install $1" >> "${FEEDS_LOG}";;
		esac
		;;
esac
exit 0
EOF
chmod +x "$sdk/scripts/feeds"
export FEEDS_LOG="$work/feeds.log"
: > "$FEEDS_LOG"

rc=0
run_build x86 64 || rc=$?
[ "$rc" -ne 0 ] || { not_ok 'the build stops before defconfig without a make'; }

if grep -q 'install -a' "$FEEDS_LOG"; then
	not_ok '"feeds install -a" widens the build graph'
else
	ok '"feeds install -a" is not used'
fi
if grep -q 'update -a' "$FEEDS_LOG"; then
	not_ok '"feeds update -a" is not used'
else
	ok '"feeds update -a" is not used'
fi

# Every required package must be installed with the documented syntax.
# "feeds install base/libjson-c" does NOT work: the tool looks for a package
# literally named "base/libjson-c" and only warns when it is missing.
missing=''
for required in \
	base:libjson-c base:ca-bundle base:uhttpd base:firewall4 \
	base:nftables-json base:rpcd base:uci base:ubus base:jsonfilter \
	base:jshn base:openssl-util \
	packages:libsqlite3 packages:curl packages:conntrack \
	packages:libmicrohttpd-no-ssl \
	luci:luci-base luci:rpcd-mod-luci
do
	feed=${required%%:*}
	package=${required##*:}
	if ! grep -qx "install -p $feed $package" "$FEEDS_LOG"; then
		missing="$missing $required"
	fi
done
if [ -z "$missing" ]; then
	ok 'every required feed package is installed with "-p feed package"'
else
	not_ok "these feed packages are missing or use the wrong syntax:$missing"
	cat "$FEEDS_LOG" >&2
fi

# The broken "feed/package" syntax must not appear: the feeds tool looks for
# a package literally named "base/libjson-c" and only warns when it is
# missing, so the build would silently continue with a smaller package set.
if grep -q 'install base/' "$FEEDS_LOG"; then
	not_ok 'the broken "feed/package" install syntax is not used'
else
	ok 'the broken "feed/package" install syntax is not used'
fi

# No libustream variant may be selected: the three variants are mutually
# exclusive and install the same file, so choosing one here would decide the
# TLS backend of the produced image.
if grep -q 'libustream' "$FEEDS_LOG"; then
	not_ok 'no libustream variant is selected by the build'
	cat "$FEEDS_LOG" >&2
else
	ok 'no libustream variant is selected by the build'
fi

# The stock opennds recipe and the routing feed are not used: the gated
# local odac-opennds recipe replaces them.
if grep -q 'opennds\|update routing' "$FEEDS_LOG"; then
	not_ok 'the stock opennds recipe and the routing feed are not used'
	cat "$FEEDS_LOG" >&2
else
	ok 'the stock opennds recipe and the routing feed are not used'
fi
if [ -f "$sdk/package/odac/odac-opennds/files/etc/init.d/opennds" ]; then
	ok 'the gated odac-opennds recipe is copied into the SDK'
else
	not_ok 'the gated odac-opennds recipe is copied into the SDK'
fi

# --- the configuration is verified after defconfig ---------------------------

# A "make" stub that emulates the part of defconfig that matters: Kconfig
# writes a disabled boolean as a comment, not as "=n".
mkdir -p "$work/bin"
cat > "$work/bin/make" <<'EOF'
#!/bin/sh
case "$1" in
	defconfig)
		# Check the INPUT before emulating dependency resolution. The old
		# stub invented the desired selection, concealing missing inputs.
		for symbol in ALL ALL_NONSHARED ALL_KMODS; do
			grep -Fqx "# CONFIG_${symbol} is not set" .config || exit 43
		done
		for package in libargon2 odac-opennds device-access luci-app-device-access; do
			grep -Fqx "CONFIG_PACKAGE_${package}=m" .config || exit 44
		done
		for symbol in \
			PACKAGE_libargon2 PACKAGE_odac-opennds PACKAGE_device-access PACKAGE_luci-app-device-access \
			PACKAGE_libsqlite3 PACKAGE_libjson-c PACKAGE_ca-bundle PACKAGE_uhttpd \
			PACKAGE_libmicrohttpd-no-ssl PACKAGE_firewall4 PACKAGE_nftables-json PACKAGE_conntrack \
			PACKAGE_rpcd PACKAGE_uci PACKAGE_ubus PACKAGE_jshn PACKAGE_jsonfilter \
			PACKAGE_curl PACKAGE_openssl-util PACKAGE_luci-base PACKAGE_rpcd-mod-luci
		do
			printf 'CONFIG_%s=m\n' "$symbol" >> .config
		done
		[ "${STOCK_OPENNDS:-0}" = 0 ] || printf 'CONFIG_PACKAGE_opennds=m\n' >> .config
		mkdir -p tmp
		echo '# generated dependency graph' > tmp/.packagedeps
		exit 0
		;;
	*/compile)
		# Record the whole command line and emulate the packages that one
		# goal produces through its dependencies.
		echo "$*" >> "${MAKE_LOG}"
		mkdir -p bin/packages/x86_64/base
		for package in libargon2-20190702-r1 odac-opennds-10.3.1-r1 \
			device-access-0.1.0-r1 luci-app-device-access-0.1.0-r1
		do
			file="bin/packages/x86_64/base/${package}.apk"
			case "$package" in
				"${MISSING_PACKAGE:-none}"-[0-9]*) rm -f "$file";;
				*) : > "$file";;
			esac
		done
		exit 0
		;;
esac
exit 0
EOF
chmod +x "$work/bin/make"
export MAKE_LOG="$work/make.log"
: > "$MAKE_LOG"
mkdir "$work/diagnostics"

rc=0
PATH="$work/bin:$PATH" run_build x86 64 ODAC_BUILD_JOBS=3 ODAC_BUILD_DIAGNOSTICS="$work/diagnostics" || rc=$?
if [ "$rc" -eq 0 ]; then
	ok 'the build accepts a configuration with CONFIG_ALL disabled'
else
	not_ok 'the build accepts a configuration with CONFIG_ALL disabled'
	cat "$work/output" >&2
fi

# Separate make runs repeated dependency work, so the packages are built by
# exactly one run whose goal reaches all four, with an explicit job count.
if [ "$(wc -l < "$MAKE_LOG")" -eq 1 ] &&
	grep -qx 'package/odac/luci-app-device-access/compile V=s -j3' "$MAKE_LOG"; then
	ok 'the packages are built by one make run with the requested job count'
else
	not_ok 'the packages are built by one make run with the requested job count'
	cat "$MAKE_LOG" >&2
fi

# The configuration before and after defconfig and the generated dependency
# graph are kept as build evidence.
if grep -Fqx 'CONFIG_PACKAGE_device-access=m' "$work/diagnostics/sdk-config-input" &&
	! grep -Fq 'CONFIG_PACKAGE_curl' "$work/diagnostics/sdk-config-input" &&
	grep -Fqx 'CONFIG_PACKAGE_curl=m' "$work/diagnostics/sdk-config" &&
	[ -s "$work/diagnostics/packagedeps.mk" ]; then
	ok 'the configuration before and after defconfig and the dependency graph are saved'
else
	not_ok 'the configuration before and after defconfig and the dependency graph are saved'
	ls -l "$work/diagnostics" >&2
fi

# The single goal reaches three packages through dependencies. A package the
# run did not produce must fail the build here instead of surfacing later.
for missing in libargon2 odac-opennds device-access luci-app-device-access; do
	rc=0
	PATH="$work/bin:$PATH" run_build x86 64 ODAC_BUILD_JOBS=1 MISSING_PACKAGE="$missing" || rc=$?
	if [ "$rc" -ne 0 ] && grep -q "No package matching ${missing}-" "$work/output"; then
		ok "a build that does not produce ${missing} is rejected"
	else
		not_ok "a build that does not produce ${missing} is rejected (exit $rc)"
		cat "$work/output" >&2
	fi
done

# The job count is passed to make, so it must be a plain positive number.
for jobs in 0 4x -4 '2 V=99'; do
	: > "$MAKE_LOG"
	rc=0
	PATH="$work/bin:$PATH" run_build x86 64 ODAC_BUILD_JOBS="$jobs" || rc=$?
	if [ "$rc" -ne 0 ] && [ ! -s "$MAKE_LOG" ]; then
		ok "the job count '${jobs}' is rejected before make runs"
	else
		not_ok "the job count '${jobs}' is rejected before make runs (exit $rc)"
		cat "$work/output" >&2
	fi
done

# The verification must accept the comment form Kconfig actually writes.
if grep -qx '# CONFIG_ALL is not set' "$sdk/.config"; then
	ok 'CONFIG_ALL is verified in the form Kconfig writes'
else
	not_ok 'CONFIG_ALL is verified in the form Kconfig writes'
	cat "$sdk/.config" >&2
fi

# A configuration that also selects the stock opennds package is rejected.
rc=0
PATH="$work/bin:$PATH" run_build x86 64 STOCK_OPENNDS=1 || rc=$?
if [ "$rc" -ne 0 ] && grep -q 'stock opennds' "$work/output"; then
	ok 'a configuration that selects the stock opennds package is rejected'
else
	not_ok 'a configuration that selects the stock opennds package is rejected'
	cat "$work/output" >&2
fi

# A configuration in which CONFIG_ALL is still on must be rejected: the build
# would compile every package of every feed.
cat > "$work/bin/make" <<'EOF'
#!/bin/sh
case "$1" in
	defconfig)
		sed -i '/^CONFIG_ALL=/d; /^# CONFIG_ALL is not set$/d' .config
		printf 'CONFIG_ALL=y\n' >> .config
		printf 'CONFIG_PACKAGE_libargon2=m\nCONFIG_PACKAGE_odac-opennds=m\nCONFIG_PACKAGE_device-access=m\nCONFIG_PACKAGE_luci-app-device-access=m\n' >> .config
		exit 0
		;;
esac
exit 0
EOF
chmod +x "$work/bin/make"
rc=0
PATH="$work/bin:$PATH" run_build x86 64 || rc=$?
if [ "$rc" -ne 0 ]; then
	ok 'a configuration with CONFIG_ALL enabled is rejected'
else
	not_ok 'a configuration with CONFIG_ALL enabled is rejected'
	cat "$work/output" >&2
fi

# A configuration in which one of the three ODAC packages was not selected
# must be rejected as well.
cat > "$work/bin/make" <<'EOF'
#!/bin/sh
case "$1" in
	defconfig)
		sed -i '/^CONFIG_ALL=/d; /^# CONFIG_ALL is not set$/d' .config
		sed -i '/^CONFIG_PACKAGE_device-access=/d; /^CONFIG_PACKAGE_luci-app-device-access=/d' .config
		printf '# CONFIG_ALL is not set\n' >> .config
		printf 'CONFIG_PACKAGE_libargon2=m\n' >> .config
		exit 0
		;;
esac
exit 0
EOF
chmod +x "$work/bin/make"
rc=0
PATH="$work/bin:$PATH" run_build x86 64 || rc=$?
if [ "$rc" -ne 0 ]; then
	ok 'a configuration without the ODAC packages is rejected'
else
	not_ok 'a configuration without the ODAC packages is rejected'
	cat "$work/output" >&2
fi

printf '\n# pass=%s fail=%s\n' "$PASS" "$FAILURES"
[ "$FAILURES" -eq 0 ] || exit 1
