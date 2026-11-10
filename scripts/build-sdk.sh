#!/bin/sh
# Run under Linux/WSL with an explicitly selected, already extracted SDK.
set -eu
[ "$#" -eq 3 ] || { echo 'Usage: build-sdk.sh SDK_DIR TARGET SUBTARGET' >&2; exit 2; }
sdk=$(realpath "$1")
target=$2; subtarget=$3
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ "$(uname -s)" = Linux ] || { echo 'Official SDK requires Linux' >&2; exit 1; }
[ -f "$sdk/include/package-pack.mk" ] && [ -x "$sdk/scripts/feeds" ] || { echo 'Not an extracted OpenWrt SDK' >&2; exit 1; }
# Require explicit 25.12 release and exact selected target in SDK metadata.
[ -f "$sdk/.config" ] || { echo 'SDK .config missing' >&2; exit 1; }
# A released SDK ships no CONFIG_VERSION_NUMBER; the substituted release lives
# in include/version.mk as the default of VERSION_NUMBER. Honour an explicit
# option when the caller set one, otherwise read that default.
version=$(sed -n 's/^VERSION_NUMBER:=\$(if \$(VERSION_NUMBER),\$(VERSION_NUMBER),\([^)]*\))$/\1/p' "$sdk/include/version.mk")
if grep -Eq '^CONFIG_VERSION_NUMBER="' "$sdk/.config"; then
	version=$(sed -n 's/^CONFIG_VERSION_NUMBER="\(.*\)"$/\1/p' "$sdk/.config")
fi
case "$version" in
	25.12.*) ;;
	*) echo "SDK must identify OpenWrt 25.12.x, found '${version:-nothing}'" >&2; exit 1;;
esac
grep -Fqx "CONFIG_TARGET_BOARD=\"$target\"" "$sdk/.config" || { echo 'SDK target mismatch' >&2; exit 1; }
grep -Fqx "CONFIG_TARGET_SUBTARGET=\"$subtarget\"" "$sdk/.config" || { echo 'SDK subtarget mismatch' >&2; exit 1; }
[ ! -e "$sdk/package/odac" ] || { echo 'SDK package/odac already exists; review before replacement' >&2; exit 1; }
mkdir "$sdk/package/odac"
for package in argon2 device-access luci-app-device-access; do cp -R "$project/$package" "$sdk/package/odac/"; done
cd "$sdk"
# Keep the selected release feed revisions; do not rewrite feeds.conf.default.
./scripts/feeds update -a
./scripts/feeds install -a
printf '\nCONFIG_USE_APK=y\nCONFIG_PACKAGE_libargon2=m\nCONFIG_PACKAGE_device-access=m\nCONFIG_PACKAGE_luci-app-device-access=m\n' >> .config
make defconfig
for package in libargon2 device-access luci-app-device-access; do
 grep -Fqx "CONFIG_PACKAGE_$package=m" .config || { echo "Package not resolved: $package" >&2; exit 1; }
done
make package/odac/argon2/compile V=s
make package/odac/device-access/compile V=s
make package/odac/luci-app-device-access/compile V=s
find bin/packages -type f \( -name 'libargon2-*.apk' -o -name 'device-access-*.apk' -o -name 'luci-app-device-access-*.apk' \) -print
