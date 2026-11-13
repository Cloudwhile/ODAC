#!/bin/sh
# Fast pre-flight checks that run before any cross-compilation job.
#
# This stage exists to fail fast: it downloads no SDK and builds no firmware.
# It compiles and tests the portable part of the sources with the host
# toolchain, so a broken core, a broken JSON boundary or a broken build
# script is caught in about a minute instead of after a 40 minute SDK
# download and build.
#
# What it does:
#   1. shell syntax check of every shell script in the repository
#   2. native CMake configure and build with the host tests enabled
#   3. CTest run of the core account/session tests and the JSON boundary
#      tests
#   4. the build-script unit tests (tests/build_sdk_test.sh)
#   5. the workflow configuration check (tests/workflow_check.py)
#   6. the build scope resolution tests (tests/scope_test.sh)
#   7. the lifecycle regression tests (tests/lifecycle_test.sh)
#   8. the enable transaction tests (scripts/test-deployment.py)
#
# What it deliberately does NOT do:
#   - it does not download the OpenWrt SDK
#   - it does not skip an assertion or ignore an exit status: every stage
#     propagates its exit status, and the script stops at the first failure
#
# The platform and nftables tests need kernel namespaces and are attempted
# only when the environment supports them. When they cannot run, that is
# reported explicitly as SKIPPED, never as PASSED.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

log() { printf '%s\n' "$*"; }
stage() { printf '\n=== %s ===\n' "$*"; }
fail() { printf 'fast-check: %s\n' "$*" >&2; exit 1; }

BUILD=${ODAC_FAST_BUILD_DIR:-"$root/build-fast"}
CC_BIN=${CC:-cc}

# --- 1. shell syntax --------------------------------------------------------

stage 'Shell syntax check'
shells=$(find . -path ./build-fast -prune -o -path ./.git -prune -o -path ./.work -prune -o \
	-name '*.sh' -type f -print | sort)
count=0
for script in $shells; do
	sh -n "$script" || fail "shell syntax error in $script"
	count=$((count + 1))
done
log "checked $count shell scripts"

# The runtime scripts under /usr/libexec and the uci-defaults scripts carry no
# .sh suffix, so they are checked by name as well. A syntax error in one of
# them would only surface on the router.
runtime=$(find device-access/files odac-opennds/files -type f \( -path '*/usr/libexec/*' -o -path '*/etc/uci-defaults/*' -o -path '*/etc/init.d/*' \) | sort)
for script in $runtime; do
	sh -n "$script" || fail "shell syntax error in $script"
	count=$((count + 1))
done
log "checked $count shell scripts in total"

# --- 2. native CMake configure and build ------------------------------------

stage 'Native CMake configure'
# The build directory is reused when it already exists, so an incremental run
# stays fast. A directory that was created for a different source tree is
# removed, because CMake would otherwise fail on a stale cache.
if [ -f "$BUILD/CMakeCache.txt" ]; then
	cached=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD/CMakeCache.txt")
	case "$cached" in
		"$root/device-access") :;;
		*)
			log "the build directory belongs to a different source tree; recreating it"
			rm -rf "$BUILD";;
	esac
else
	[ ! -e "$BUILD" ] || rm -rf "$BUILD"
fi
mkdir -p "$BUILD"
cmake -S device-access -B "$BUILD" \
	-DODAC_TESTS=ON \
	-DCMAKE_BUILD_TYPE=Debug \
	-DCMAKE_C_COMPILER="$CC_BIN" \
	|| fail 'native CMake configure failed'

stage 'Native build'
cmake --build "$BUILD" --parallel "$(nproc 2>/dev/null || echo 2)" || fail 'native build failed'

# --- 3. CTest ---------------------------------------------------------------

stage 'CTest'
ctest --test-dir "$BUILD" --output-on-failure || fail 'CTest failed'

# --- 4. build script unit tests ---------------------------------------------

stage 'Build script unit tests'
sh tests/build_sdk_test.sh || fail 'build script unit tests failed'

# --- 5. workflow configuration ------------------------------------------------

stage 'Workflow configuration check'
if python3 -c 'import yaml' 2>/dev/null; then
	python3 tests/workflow_check.py .github/workflows/build-apk.yml scripts/targets.txt ||
		fail 'the workflow configuration is not valid'
	# The negative test proves that the checker actually rejects the bugs
	# it is meant to catch. Without it, a check that silently stopped
	# working would look exactly like a passing check.
	sh tests/workflow_negative_test.sh || fail 'the workflow check does not reject the known broken variants'
else
	fail 'PyYAML is required for the workflow check'
fi

stage 'APK content checker regression tests'
python3 -I tests/apk_content_test.py || fail 'APK content checker tests failed'
python3 -I tests/apk_elf_test.py || fail 'APK payload and ELF checker tests failed'
python3 -I tests/opennds_dns_test.py || fail 'openNDS DNS isolation tests failed'

# --- 6. build scope resolution ------------------------------------------------

stage 'Build scope resolution'
# The scope decides which architectures are built. The matrix content is
# checked, not only the scope label: a scope that resolved to the full list
# on a pull request would silently spend the whole CI budget.
sh tests/scope_test.sh || fail 'the build scope resolution is not correct'

# --- 7. lifecycle regression tests -------------------------------------------

stage 'Lifecycle regression tests'
if [ "$(id -u)" = 0 ]; then
	sh tests/lifecycle_test.sh || fail 'lifecycle regression tests failed'
else
	# The runtime scripts refuse to run as a non-root user, and one of the
	# assertions checks that the ownership file is root owned. The suite is
	# skipped here and reported as skipped, never as passed.
	log 'SKIPPED: tests/lifecycle_test.sh requires root; run this stage with sudo'
fi

# The enable transaction suite runs the real odac-setup in a temporary
# sandbox with every external command stubbed; it needs no root.
stage 'Enable transaction tests'
python3 scripts/test-deployment.py || fail 'enable transaction tests failed'
sh tests/network_config_test.sh || fail 'network configuration RPC tests failed'
python3 -I tests/config_job_test.py || fail 'configuration job tests failed'
node --test tests/portal_request_test.cjs tests/settings_view_test.cjs tests/config_job_poll_test.cjs || fail 'frontend request and settings tests failed'

# --- 8. platform and nftables tests -----------------------------------------

stage 'Platform and nftables tests'
if [ "${ODAC_RUN_PLATFORM_TESTS:-0}" = 1 ]; then
	sh tests/run_platform_tests.sh || fail 'platform tests failed'
else
	if command -v unshare >/dev/null 2>&1 && unshare -rnm true >/dev/null 2>&1; then
		log 'Running the platform tests: unprivileged user namespaces are available'
		sh tests/run_platform_tests.sh || fail 'platform tests failed'
	else
		log 'SKIPPED: the platform and nftables tests need unprivileged user'
		log 'namespaces (unshare -rnm), which this runner does not provide.'
		log 'These tests are NOT reported as passed.'
	fi
fi

printf '\nfast-check: all executed stages passed\n'
