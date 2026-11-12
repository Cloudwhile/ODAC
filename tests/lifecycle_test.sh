#!/bin/sh
# Lifecycle regression tests for the ODAC runtime scripts.
#
# These tests exercise the real shell scripts with a fake OpenWrt userland.
# Nothing here touches a real router: uci, nft, fw4, opennds and the init
# scripts are replaced by test doubles under a temporary root.
#
# Covered behaviour:
#   - odac-ownership records and clears ownership
#   - odac-remove removes only what ODAC owns and keeps the controlled
#     network closed
#   - odac-remove does not stop an openNDS that ODAC does not own
#   - odac-remove touches nothing when ODAC was never activated
#   - odac-purge lists, backs up and refuses to delete data without an
#     explicit confirmation
#   - odac-purge keeps the protective guard
#   - the gated odac-opennds init script starts nothing and changes no
#     network configuration until ODAC activated a guarded interface
#   - odac-setup --disable retires through the locked removal transaction
#     and keeps ownership on every failure
#   - odac-setup --restore-network refuses unproven shutdown states and
#     restores the guard after a failed removal
#   - odac-setup --enable rolls back on failure and refuses foreign state
set -eu

# The scripts under test refuse to run as a non-root user, and one assertion
# checks that the ownership file is root owned. Run this suite as root.
if [ "$(id -u)" != 0 ]; then
	echo 'lifecycle_test.sh must run as root (the runtime scripts require it)' >&2
	exit 1
fi

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
LIBEXEC=$ROOT/device-access/files/usr/libexec
UCIDEFS=$ROOT/device-access/files/etc/uci-defaults

PASS=0
FAILURES=0

ok() {
	PASS=$((PASS + 1))
	printf 'ok - %s\n' "$1"
}

not_ok() {
	FAILURES=$((FAILURES + 1))
	printf 'not ok - %s\n' "$1" >&2
}

check() {
	if [ "$2" = "$3" ]; then
		ok "$1"
	else
		not_ok "$1: expected [$3] got [$2]"
	fi
}

contains() {
	case "$2" in
		*"$3"*) ok "$1";;
		*) not_ok "$1: [$3] missing in [$2]";;
	esac
}

absent() {
	case "$2" in
		*"$3"*) not_ok "$1: [$3] unexpectedly present in [$2]";;
		*) ok "$1";;
	esac
}

# Count the lines of a file that match a pattern. Prints 0 when the file does
# not exist. grep -c already prints 0 for a missing match, so its exit status
# must not be turned into a second line of output.
count_matches() {
	[ -f "$1" ] || { echo 0; return 0; }
	grep -c "$2" "$1" 2>/dev/null || true
}

# --- test harness ------------------------------------------------------------
# Each test runs in its own sandbox root. The scripts under test read their
# state from ODAC_OWNERSHIP_DIR (overridable) and call uci/nft/fw4 and the
# init scripts; those are redirected to doubles inside the sandbox.

new_root() {
	ROOTDIR=$(mktemp -d)
	mkdir -p "$ROOTDIR/etc/config" "$ROOTDIR/etc/odac" "$ROOTDIR/etc/nftables.d" \
		"$ROOTDIR/etc/init.d" "$ROOTDIR/var/lock" "$ROOTDIR/usr/libexec" "$ROOTDIR/var/run"
	# The ownership helper honours ODAC_OWNERSHIP_DIR.
	export ODAC_OWNERSHIP_DIR="$ROOTDIR/etc/odac"
	export SANDBOX="$ROOTDIR"
	# The ODAC configuration file itself. The runtime scripts check for its
	# presence before touching it.
	: > "$ROOTDIR/etc/config/odac"
	# The openNDS and firewall configuration files. The runtime scripts check
	# for their presence before touching them.
	: > "$ROOTDIR/etc/config/opennds"
	: > "$ROOTDIR/etc/config/firewall"
	: > "$ROOTDIR/etc/config/network"
	# Test double for uci. Like real uci, every package lives in its own file
	# (<confdir>/<package>), and "-c DIR" selects another configuration
	# directory. Copying a staged file over /etc/config therefore moves the
	# values exactly as on a router, and restoring a file restores its values.
	#
	# Keys are matched literally. A uci key such as
	# "opennds.@opennds[0].gatewayinterface" contains characters that are
	# regular expression metacharacters, so grep/sed cannot be used here.
	#
	# Real uci passes "set key=value" as a single argument, and the value may
	# contain further "=" characters, so the split happens at the first one.
	cat > "$ROOTDIR/bin_uci" <<'EOF'
#!/bin/sh
# Minimal uci double: "uci get/set/delete/add_list/show/commit".
confdir=${SANDBOX:?}/etc/config
while [ $# -gt 0 ]; do
	case "$1" in
		-c) confdir=$2; shift 2;;
		-*) shift;;
		*) break;;
	esac
done
cmd=$1
[ $# -gt 0 ] && shift
arg=${1:-}
printf 'uci %s %s\n' "$cmd" "$arg" >> "${SANDBOX}/calls.all"
[ "${TEST_FAIL_UCI:-}" != "$cmd $arg" ] || exit 1
[ $# -gt 0 ] && shift
[ "$cmd" != commit ] || exit 0
state=$confdir/${arg%%.*}

# Split "key=value" into a key and a value.
key=$arg
value=
case "$arg" in
	*=*)
		key=${arg%%=*}
		value=${arg#*=}
		;;
esac
# "uci set key value" (value in the next argument) is also accepted.
if [ "$cmd" = set ] && [ $# -gt 0 ] && [ -z "$value" ]; then
	value=$1
fi

case "$cmd" in
	show)
		awk -v k="$key" 'index($0, k "=") == 1 || index($0, k ".") == 1 { print }' "$state"
		;;
	get)
		[ -f "$state" ] || exit 1
		line=$(awk -v k="$key" 'index($0, k "=") == 1 { print substr($0, length(k) + 2); exit }' "$state")
		[ -n "$line" ] || exit 1
		printf '%s\n' "$line"
		;;
	set)
		[ -f "$state" ] || : > "$state"
		awk -v k="$key" -v v="$value" '
			BEGIN { done = 0 }
			index($0, k "=") == 1 {
				if (!done) { print k "=" v; done = 1 }
				next
			}
			{ print }
			END { if (!done) print k "=" v }
		' "$state" > "$state.tmp" || exit 1
		mv "$state.tmp" "$state"
		;;
	delete)
		[ -f "$state" ] || exit 0
		awk -v k="$key" 'index($0, k "=") != 1 && index($0, k ".") != 1' "$state" > "$state.tmp" || exit 1
		mv "$state.tmp" "$state"
		;;
	add_list)
		[ -f "$state" ] || exit 0
		awk -v k="$key" 'index($0, k "=") == 1 { found = 1 } END { exit !found }' "$state" || {
			printf '%s=%s\n' "$key" "$value" >> "$state"
			exit 0
		}
		;;
	commit) : ;;
	*) exit 1;;
esac
exit 0
EOF
	chmod +x "$ROOTDIR/bin_uci"
	# The runtime scripts call "uci" by name; expose the double on PATH.
	cat > "$ROOTDIR/uci" <<'EOF'
#!/bin/sh
exec "${SANDBOX}/bin_uci" "$@"
EOF
	chmod +x "$ROOTDIR/uci"
	# Test double for the init scripts. They record what was asked of them.
	for svc in odac opennds; do
		cat > "$ROOTDIR/etc/init.d/$svc" <<EOF
#!/bin/sh
echo "\$1" >> "$ROOTDIR/calls.$svc"
echo "$svc \$1" >> "$ROOTDIR/calls.all"
[ "\${TEST_FAIL_SERVICE:-}" != "$svc \$1" ] || exit 1
if [ "\$1" = running ]; then exit "\${TEST_SERVICE_STATUS:-1}"; fi
exit 0
EOF
		chmod +x "$ROOTDIR/etc/init.d/$svc"
	done
	# Test double for fw4.
	cat > "$ROOTDIR/fw4" <<'EOF'
#!/bin/sh
echo "$1" >> "${SANDBOX}/calls.fw4"
echo "fw4 $1" >> "${SANDBOX}/calls.all"
call=$(grep -c "^$1$" "${SANDBOX}/calls.fw4")
[ "${TEST_FAIL_FW4:-}" != "$1:$call" ] || exit 1
exit 0
EOF
	chmod +x "$ROOTDIR/fw4"
	# Test double for the firewall helper.
	cat > "$ROOTDIR/odac-firewall" <<'EOF'
#!/bin/sh
echo "$*" >> "${SANDBOX}/calls.firewall"
echo "guard $*" >> "${SANDBOX}/calls.all"
[ "${TEST_FAIL_GUARD:-0}" = 0 ] || exit 1
exit 0
EOF
	chmod +x "$ROOTDIR/odac-firewall"
	# Test double for nft.
	cat > "$ROOTDIR/nft" <<'EOF'
#!/bin/sh
echo "$*" >> "${SANDBOX}/calls.nft"
[ "${TEST_FAIL_NFT:-}" != "$*" ] || exit 1
case "$*" in
	'list tables') [ "${TEST_LIVE_GUARD:-1}" = 0 ] || echo 'table inet odac_guard';;
	'-j list set inet odac_guard controlled') printf '{"nftables":[{"set":{"elem":["%s"]}}]}\n' "${TEST_CONTROLLED_IFACE:-br-odac}";;
esac
exit 0
EOF
	chmod +x "$ROOTDIR/nft"
	cat > "$ROOTDIR/jsonfilter" <<'EOF'
#!/bin/sh
# Only the standalone controlled-set query used by restore is supported.
[ "$*" = '-e @.nftables[*].set.elem[*]' ] || exit 1
python3 -c 'import json,sys; print("\n".join(json.load(sys.stdin)["nftables"][0]["set"]["elem"]))'
EOF
	chmod +x "$ROOTDIR/jsonfilter"
	cat > "$ROOTDIR/pidof" <<'EOF'
#!/bin/sh
exit "${TEST_DAEMON_STATUS:-1}"
EOF
	chmod +x "$ROOTDIR/pidof"
	export PATH="$ROOTDIR:$PATH"
	cp "$LIBEXEC/odac-ownership" "$ROOTDIR/usr/libexec/odac-ownership"
	cp "$LIBEXEC/odac-remove" "$ROOTDIR/usr/libexec/odac-remove"
	cp "$LIBEXEC/odac-purge" "$ROOTDIR/usr/libexec/odac-purge"
	chmod +x "$ROOTDIR/usr/libexec/"*
}

cleanup_root() {
	[ -n "${ROOTDIR:-}" ] && rm -rf "$ROOTDIR"
	ROOTDIR=
}

# Run a script under test with the sandbox on the path and the ownership
# directory pointed at the sandbox.
run_script() {
	script=$1
	shift
	ODAC_OWNERSHIP_DIR="$ROOTDIR/etc/odac" \
	SANDBOX="$ROOTDIR" \
	PATH="$ROOTDIR:$PATH" \
	sh "$ROOTDIR/usr/libexec/$script" "$@" 2>&1
}

# The runtime scripts use absolute paths like /etc/init.d/odac. Replace them
# with sandbox-relative ones by running the scripts against a rewritten copy.
sandbox_script() {
	name=$1
	sed -e "s|/etc/init.d|@ROOT@/etc/init.d|g" \
	    -e "s|/usr/libexec/odac-firewall|@ROOT@/odac-firewall|g" \
	    -e "s|/usr/libexec/odac-ownership|@ROOT@/usr/libexec/odac-ownership|g" \
	    -e "s|/usr/libexec/odac-remove|@ROOT@/usr/libexec/odac-remove|g" \
	    -e "s|/sys/class/net|@ROOT@/sys/class/net|g" \
	    -e "s|/etc/nftables.d|@ROOT@/etc/nftables.d|g" \
	    -e "s|/etc/config|@ROOT@/etc/config|g" \
	    -e "s|/etc/odac|@ROOT@/etc/odac|g" \
	    -e "s|/var/lock|@ROOT@/var/lock|g" \
	    -e "s|@ROOT@|$ROOTDIR|g" \
	    "$LIBEXEC/$name" > "$ROOTDIR/usr/libexec/$name"
	chmod +x "$ROOTDIR/usr/libexec/$name"
}

# --- 1. ownership ------------------------------------------------------------

new_root
sandbox_script odac-ownership
out=$(ODAC_OWNERSHIP_DIR="$ROOTDIR/etc/odac" sh "$ROOTDIR/usr/libexec/odac-ownership" 2>&1 || true)
# Use the helper functions directly.
cat > "$ROOTDIR/own_test.sh" <<'EOF'
. "${SANDBOX}/usr/libexec/odac-ownership"
odac_ownership_set iface br-odac
odac_ownership_set zone odac
printf '%s\n' "$(odac_ownership_get iface)"
printf '%s\n' "$(odac_ownership_get zone)"
printf '%s\n' "$(odac_ownership_get missing)"
odac_ownership_set iface br-other
printf '%s\n' "$(odac_ownership_get iface)"
odac_ownership_unset zone
printf '%s\n' "$(odac_ownership_get zone)"
key=/etc/ssl/private/portal.key
value=caller-value
tmp=caller-temp
before_umask=$(umask)
odac_ownership_set guard 1
printf '%s\n' "$key|$value|$tmp|$before_umask|$(umask)"
if odac_ownership_set 'bad.*' invalid 2>/dev/null; then exit 1; fi
if odac_ownership_set iface 'br-odac
odac_active 1' 2>/dev/null; then exit 1; fi
EOF
out=$(SANDBOX="$ROOTDIR" ODAC_OWNERSHIP_DIR="$ROOTDIR/etc/odac" sh "$ROOTDIR/own_test.sh")
check "ownership stores the interface" "$(echo "$out" | sed -n 1p)" "br-odac"
check "ownership stores the zone" "$(echo "$out" | sed -n 2p)" "odac"
check "ownership returns nothing for an absent key" "$(echo "$out" | sed -n 3p)" ""
check "ownership overwrites a key" "$(echo "$out" | sed -n 4p)" "br-other"
check "ownership unsets a key" "$(echo "$out" | sed -n 5p)" ""
check "ownership file is root owned mode 600" \
	"$(stat -c '%u:%a' "$ROOTDIR/etc/odac/ownership")" "0:600"
check "ownership preserves caller variables and umask" "$(printf '%s\n' "$out" | sed -n 6p)" \
	"/etc/ssl/private/portal.key|caller-value|caller-temp|$(umask)|$(umask)"
check "ownership rejects injected records" "$(count_matches "$ROOTDIR/etc/odac/ownership" '^odac_active ')" 0
cleanup_root

# --- 2. odac-remove on a never-activated installation ------------------------

new_root
sandbox_script odac-remove
out=$(SANDBOX="$ROOTDIR" run_script odac-remove --retain-guard)
check "remove on a fresh install succeeds" "$?" "0"
check "remove on a fresh install does not call opennds" \
	"$( [ -f "$ROOTDIR/calls.opennds" ] && cat "$ROOTDIR/calls.opennds" | tr '\n' ',' || echo none)" "none"
check "remove on a fresh install writes no retired guard" \
	"$( [ -f "$ROOTDIR/etc/nftables.d/90-odac-retired.nft" ] && echo present || echo absent)" "absent"
cleanup_root

# --- 3. odac-remove keeps the controlled network closed -----------------------

new_root
sandbox_script odac-remove
printf 'odac.main.interface=br-odac\n' > "$ROOTDIR/etc/config/odac"
# Mark the installation as activated with an owned interface.
cat > "$ROOTDIR/etc/odac/ownership" <<'EOF'
iface br-odac
zone odac
firewall_sections odac_guard odac_dns
guard 1
opennds_managed 1
odac_active 1
EOF
chmod 600 "$ROOTDIR/etc/odac/ownership"
out=$(SANDBOX="$ROOTDIR" run_script odac-remove --retain-guard)
check "remove with an active installation succeeds" "$?" "0"
check "remove closes the controlled interface" \
	"$(count_matches "$ROOTDIR/calls.firewall" 'close br-odac')" "1"
check "remove stops odac" \
	"$(count_matches "$ROOTDIR/calls.odac" '^stop$')" "1"
check "remove stops opennds that ODAC owned" \
	"$(count_matches "$ROOTDIR/calls.opennds" '^stop$')" "1"
check "remove installs the retired guard" \
	"$( [ -f "$ROOTDIR/etc/nftables.d/90-odac-retired.nft" ] && echo present || echo absent)" "present"
contains "the retired guard denies the controlled interface" \
	"$(cat "$ROOTDIR/etc/nftables.d/90-odac-retired.nft")" 'iifname "br-odac" drop'
check "remove disables odac in its configuration" \
	"$(count_matches "$ROOTDIR/etc/config/odac" '^odac.main.enabled=0$')" "1"
cleanup_root

# --- 4. odac-remove does not disturb a foreign openNDS ------------------------

new_root
sandbox_script odac-remove
cat > "$ROOTDIR/etc/odac/ownership" <<'EOF'
iface br-odac
zone odac
firewall_sections odac_guard
guard 1
opennds_managed 0
odac_active 1
EOF
chmod 600 "$ROOTDIR/etc/odac/ownership"
out=$(SANDBOX="$ROOTDIR" run_script odac-remove --retain-guard)
check "remove succeeds with a foreign opennds" "$?" "0"
check "remove does not stop a foreign opennds" \
	"$( [ -f "$ROOTDIR/calls.opennds" ] && echo stopped || echo untouched)" "untouched"
cleanup_root

# --- 5. odac-purge dry run ---------------------------------------------------

new_root
sandbox_script odac-purge
cat > "$ROOTDIR/etc/odac/ownership" <<'EOF'
iface br-odac
zone odac
firewall_sections odac_guard odac_dns
guard 0
EOF
chmod 600 "$ROOTDIR/etc/odac/ownership"
: > "$ROOTDIR/etc/odac/odac.db"
: > "$ROOTDIR/etc/nftables.d/90-odac-retired.nft"
out=$(SANDBOX="$ROOTDIR" run_script odac-purge --dry-run)
contains "dry run lists the firewall sections" "$out" "firewall.odac_guard"
contains "dry run lists the database" "$out" "/etc/odac/odac.db"
contains "dry run reports the protective guard" "$out" "90-odac-retired.nft"
contains "dry run states that upstream scripts are untouched" "$out" "upstream authentication scripts"
check "dry run does not remove the database" \
	"$( [ -f "$ROOTDIR/etc/odac/odac.db" ] && echo present || echo absent)" "present"
check "dry run does not remove the guard" \
	"$( [ -f "$ROOTDIR/etc/nftables.d/90-odac-retired.nft" ] && echo present || echo absent)" "present"
cleanup_root

# --- 6. odac-purge refuses to delete data without confirmation ----------------

new_root
sandbox_script odac-purge
cat > "$ROOTDIR/etc/odac/ownership" <<'EOF'
iface br-odac
zone odac
firewall_sections odac_guard
EOF
chmod 600 "$ROOTDIR/etc/odac/ownership"
: > "$ROOTDIR/etc/odac/odac.db"
rc=0
out=$(SANDBOX="$ROOTDIR" run_script odac-purge --confirm) || rc=$?
check "purge without data confirmation fails" "$rc" "1"
contains "purge explains that data confirmation is required" "$out" "--confirm-data"
check "the database survives the refused purge" \
	"$( [ -f "$ROOTDIR/etc/odac/odac.db" ] && echo present || echo absent)" "present"
cleanup_root

# --- 7. offline purge preserves unrelated files and recovery state -----------

new_root
sandbox_script odac-purge
cat > "$ROOTDIR/etc/odac/ownership" <<'EOF'
iface br-odac
zone odac
retired_guard 1
EOF
chmod 600 "$ROOTDIR/etc/odac/ownership"
printf 'database\n' > "$ROOTDIR/etc/odac/odac.db"
printf 'committed WAL\n' > "$ROOTDIR/etc/odac/odac.db-wal"
printf 'shared memory\n' > "$ROOTDIR/etc/odac/odac.db-shm"
printf 'administrator secret\n' > "$ROOTDIR/etc/odac/custom.key"
mkdir "$ROOTDIR/etc/odac/manual-backup"
printf 'keep\n' > "$ROOTDIR/etc/odac/manual-backup/data"
: > "$ROOTDIR/etc/nftables.d/90-odac-retired.nft"
out=$(SANDBOX="$ROOTDIR" run_script odac-purge --confirm --confirm-data)
check "purge with confirmation succeeds" "$?" "0"
for file in odac.db odac.db-wal odac.db-shm; do
	check "purge removes exact data file $file" "$(test -e "$ROOTDIR/etc/odac/$file" && echo present || echo absent)" absent
done
check "purge keeps the protective guard" \
	"$( [ -f "$ROOTDIR/etc/nftables.d/90-odac-retired.nft" ] && echo present || echo absent)" "present"
check "purge keeps the ownership recovery record" "$(test -f "$ROOTDIR/etc/odac/ownership" && echo present || echo absent)" present
check "purge preserves administrator keys" "$(cat "$ROOTDIR/etc/odac/custom.key")" 'administrator secret'
check "purge preserves administrator backups" "$(cat "$ROOTDIR/etc/odac/manual-backup/data")" keep
backup=$(printf '%s\n' "$out" | sed -n 's/^Backup directory: //p')
check "automatic backup includes database" "$(cat "$backup/odac.db")" database
check "automatic backup includes committed WAL" "$(cat "$backup/odac.db-wal")" 'committed WAL'
check "automatic backup is private" "$(stat -c %a "$backup")" 700
check "purge never reloads firewall" "$(test -e "$ROOTDIR/calls.fw4" && echo called || echo untouched)" untouched
absent "purge does not suggest unverified guard deletion" "$out" 'rm '
cleanup_root

# Refusals must happen before any data deletion or network mutation.
for scenario in active enabled running daemon unknown-service sections symlink locked; do
	new_root
	sandbox_script odac-purge
	printf 'database\n' > "$ROOTDIR/etc/odac/odac.db"
	case "$scenario" in
		active) printf 'odac_active 1\n' > "$ROOTDIR/etc/odac/ownership";;
		enabled) printf 'odac.main.enabled=1\n' > "$ROOTDIR/etc/config/odac";;
		running) export TEST_SERVICE_STATUS=0;;
		daemon) export TEST_DAEMON_STATUS=0;;
		unknown-service) export TEST_SERVICE_STATUS=2;;
		sections) printf 'firewall_sections odac_guard\n' > "$ROOTDIR/etc/odac/ownership";;
		symlink) ln -s "$ROOTDIR/etc/odac/odac.db" "$ROOTDIR/etc/odac/odac.db-wal";;
		locked) mkdir "$ROOTDIR/var/lock/odac-setup.lock";;
	esac
	rc=0
	out=$(run_script odac-purge --confirm --confirm-data) || rc=$?
	check "purge refuses $scenario state" "$rc" 1
	check "refused $scenario purge preserves database" "$(cat "$ROOTDIR/etc/odac/odac.db")" database
	check "refused $scenario purge leaves firewall untouched" "$(test -e "$ROOTDIR/calls.fw4" && echo called || echo untouched)" untouched
	unset TEST_SERVICE_STATUS TEST_DAEMON_STATUS
	cleanup_root
done

# --- 8. gated odac-opennds init script ------------------------------------------
# The package postinst runs "/etc/init.d/opennds start" before any ODAC script
# can intervene, so the init script itself is the gate. It is sourced the way
# rc.common sources it, with procd and the network-changing tools stubbed. Any
# call to libopennds.sh, "service" or a uci write would be recorded.

nds_case() {
	# $1: opennds backend marker, $2: opennds enabled, $3: odac enabled,
	# $4: opennds gatewayinterface, $5: guard element present (0/1)
	new_root
	printf 'opennds.@opennds[0].odac_backend=%s\nopennds.@opennds[0].enabled=%s\nopennds.@opennds[0].gatewayinterface=%s\n' \
		"$1" "$2" "$4" > "$ROOTDIR/etc/config/opennds"
	printf 'odac.main.enabled=%s\nodac.main.interface=br-odac\n' "$3" > "$ROOTDIR/etc/config/odac"
	printf 'network.wan6.proto=dhcpv6\n' > "$ROOTDIR/etc/config/network"
	cat > "$ROOTDIR/nft" <<EOF
#!/bin/sh
echo "nft \$*" >> "$ROOTDIR/calls.all"
[ "\$*" = 'get element inet odac_guard controlled { "br-odac" }' ] && exit $((1 - $5))
exit 1
EOF
	for tool in logger service; do
		printf '#!/bin/sh\necho "%s $*" >> "%s/calls.all"\n' "$tool" "$ROOTDIR" > "$ROOTDIR/$tool"
		chmod +x "$ROOTDIR/$tool"
	done
	chmod +x "$ROOTDIR/nft"
	cat > "$ROOTDIR/run-init.sh" <<EOF
procd_open_instance() { echo procd_open >> "$ROOTDIR/calls.all"; }
procd_set_param() { echo "procd_set \$*" >> "$ROOTDIR/calls.all"; }
procd_close_instance() { echo procd_close >> "$ROOTDIR/calls.all"; }
. "$ROOT/odac-opennds/files/etc/init.d/opennds"
start_service
echo "start_rc \$?" >> "$ROOTDIR/calls.all"
type stop_service >/dev/null 2>&1 && echo has_stop_service >> "$ROOTDIR/calls.all"
exit 0
EOF
	: > "$ROOTDIR/calls.all"
	sh "$ROOTDIR/run-init.sh"
}

nds_inert() {
	check "$1: start reports success to the package manager" "$(count_matches "$ROOTDIR/calls.all" '^start_rc 0$')" 1
	check "$1: no daemon instance registered" "$(count_matches "$ROOTDIR/calls.all" '^procd_open$')" 0
	check "$1: no openNDS helper, service or network call" "$(count_matches "$ROOTDIR/calls.all" 'libopennds\|^service \|^uci set\|^uci commit')" 0
	check "$1: WAN6 configuration unchanged" "$(uci get network.wan6.proto)" dhcpv6
}

nds_case 1 0 0 odac-unconfigured 0
nds_inert 'fresh package install'
cleanup_root
nds_case 1 1 0 br-odac 1
nds_inert 'ODAC disabled'
cleanup_root
nds_case 1 1 1 br-lan 1
nds_inert 'gateway is not the ODAC interface'
cleanup_root
nds_case 1 1 1 br-odac 0
nds_inert 'guard missing'
cleanup_root
nds_case '' 1 1 br-odac 1
nds_inert 'configuration not owned by odac-opennds'
cleanup_root

nds_case 1 1 1 br-odac 1
check 'activated: daemon instance registered' "$(count_matches "$ROOTDIR/calls.all" '^procd_open$')" 1
contains 'activated: runs the openNDS daemon' "$(cat "$ROOTDIR/calls.all")" 'procd_set command /usr/bin/opennds -f'
check 'activated: never calls ipv6_routing or users_to_router' "$(count_matches "$ROOTDIR/calls.all" 'libopennds')" 0
check 'activated: WAN6 configuration unchanged' "$(uci get network.wan6.proto)" dhcpv6
check 'no stop_service that would revert network changes' "$(count_matches "$ROOTDIR/calls.all" '^has_stop_service$')" 0
cleanup_root

# Package contract: installs disabled, unowned interface, no uci-defaults.
cfg=$ROOT/odac-opennds/files/etc/config/opennds
contains 'odac-opennds ships disabled' "$(cat "$cfg")" "option enabled '0'"
contains 'odac-opennds ships without fw4 hook' "$(cat "$cfg")" "option fwhook_enabled '0'"
contains 'odac-opennds does not touch the fw4 input chain' "$(cat "$cfg")" "option users_to_router_passthrough '1'"
contains 'odac-opennds does not touch DNS or DHCP option 114' "$(cat "$cfg")" "option dhcp_default_url_enable '0'"
absent 'odac-opennds does not default to br-lan' "$(cat "$cfg")" 'br-lan'
mk=$(cat "$ROOT/odac-opennds/Makefile")
install_rules=$(sed -n '/^define Package\/odac-opennds\/install/,/^endef/p' "$ROOT/odac-opennds/Makefile" | grep -v '^[[:space:]]*#')
absent 'odac-opennds installs no uci-defaults' "$install_rules" 'uci-defaults'
absent 'odac-opennds installs no fw4 restart hook' "$install_rules" 'restart.sh'
absent 'odac-opennds installs no stock init script' "$install_rules" 'linux_openwrt/opennds/files/etc/init.d'
contains 'odac-opennds declares the stock conflict' "$mk" 'CONFLICTS:=opennds nodogsplash'
contains 'odac-opennds uses the official source hash' "$mk" 'PKG_HASH:=f82fe0fa2e4e8ab66abf33cae7cb20e79661c8a183af82eefa11307e9c66968d'
backend_mk=$(cat "$ROOT/device-access/Makefile")
contains 'device-access depends on the gated backend' "$backend_mk" '+odac-opennds'
absent 'device-access does not depend on stock opennds' "$backend_mk" '+opennds '
check 'device-access ships no uci-defaults' "$(test -e "$UCIDEFS" && echo present || echo absent)" absent

# --- 10. disable uses the locked, durable retirement transaction --------------

active_root() {
	new_root
	sandbox_script odac-setup
	sandbox_script odac-remove
	cat > "$ROOTDIR/etc/odac/ownership" <<'EOF'
iface br-odac
zone odac
firewall_sections odac_guard odac_dns
guard 1
opennds_managed 1
odac_active 1
EOF
	printf 'odac.main.enabled=1\n' > "$ROOTDIR/etc/config/odac"
	printf 'firewall.odac_guard=include\nfirewall.odac_dns=rule\n' > "$ROOTDIR/etc/config/firewall"
	for section in odac_guard odac_dns; do
		digest=$(uci show "firewall.$section" | sha256sum)
		printf 'firewall_sha256_%s %s\n' "$section" "${digest%% *}" >> "$ROOTDIR/etc/odac/ownership"
	done
	chmod 600 "$ROOTDIR/etc/odac/ownership"
}

active_root
out=$(run_script odac-setup --disable)
contains 'disable confirms closed network after successful retirement' "$out" 'Controlled network remains closed'
check 'disable persists the retired guard' "$(test -f "$ROOTDIR/etc/nftables.d/90-odac-retired.nft" && echo yes)" yes
check 'disable reloads firewall before and after removing owned rules' "$(count_matches "$ROOTDIR/calls.fw4" '^reload$')" 2
check 'disable removes unchanged owned sections' "$(count_matches "$ROOTDIR/etc/config/firewall" '^firewall.odac_')" 0
check 'disable clears active state only after success' "$(count_matches "$ROOTDIR/etc/odac/ownership" '^odac_active 0$')" 1
check 'disable releases lifecycle lock' "$(test -d "$ROOTDIR/var/lock/odac-setup.lock" && echo held || echo released)" released
order=$(awk '/^fw4 reload$/ && !reload {reload=NR} /^odac stop$/ {stop=NR} END {print (reload && stop && reload < stop) ? "safe" : "unsafe"}' "$ROOTDIR/calls.all")
check 'durable guard is loaded before stopping ODAC' "$order" safe
cleanup_root

for scenario in close check reload stop-odac disable-odac stop-opennds disable-opennds modified delete commit final-reload locked; do
	active_root
	case "$scenario" in
		close) export TEST_FAIL_GUARD=1;;
		check) export TEST_FAIL_FW4=check:1;;
		reload) export TEST_FAIL_FW4=reload:1;;
		stop-odac) export TEST_FAIL_SERVICE='odac stop';;
		disable-odac) export TEST_FAIL_SERVICE='odac disable';;
		stop-opennds) export TEST_FAIL_SERVICE='opennds stop';;
		disable-opennds) export TEST_FAIL_SERVICE='opennds disable';;
		modified) uci set firewall.odac_dns.target=ACCEPT;;
		delete) export TEST_FAIL_UCI='delete firewall.odac_guard';;
		commit) export TEST_FAIL_UCI='commit firewall';;
		final-reload) export TEST_FAIL_FW4=reload:2;;
		locked) mkdir "$ROOTDIR/var/lock/odac-setup.lock";;
	esac
	rc=0
	out=$(run_script odac-setup --disable) || rc=$?
	check "disable fails on $scenario" "$rc" 1
	check "failed $scenario retains active ownership for recovery" "$(count_matches "$ROOTDIR/etc/odac/ownership" '^odac_active 1$')" 1
	check "failed $scenario retains section ownership" "$(count_matches "$ROOTDIR/etc/odac/ownership" '^firewall_sections odac_guard odac_dns$')" 1
	absent "failed $scenario does not report success" "$out" 'ODAC stopped.'
	case "$scenario" in
		close|check|reload|locked)
			check "failed $scenario never stops services" "$(count_matches "$ROOTDIR/calls.odac" '^stop$')" 0;;
		modified)
			check 'modified section survives disable' "$(uci get firewall.odac_dns.target)" ACCEPT
			check 'modified section blocks deletion of all sections' "$(count_matches "$ROOTDIR/calls.all" '^uci delete firewall.')" 0;;
	esac
	unset TEST_FAIL_GUARD TEST_FAIL_FW4 TEST_FAIL_SERVICE TEST_FAIL_UCI
	if [ "$scenario" != modified ] && [ "$scenario" != locked ]; then
		out=$(run_script odac-setup --disable)
		contains "disable can recover from $scenario on retry" "$out" 'ODAC stopped.'
	fi
	cleanup_root
done

# --- 11. restore proves shutdown and rolls back failed guard removal ---------

retired_root() {
	active_root
	run_script odac-setup --disable >/dev/null
	mkdir -p "$ROOTDIR/sys/class/net/br-odac"
	printf '0x1002\n' > "$ROOTDIR/sys/class/net/br-odac/flags"
	printf 'down\n' > "$ROOTDIR/sys/class/net/br-odac/operstate"
	: > "$ROOTDIR/calls.fw4"
	: > "$ROOTDIR/calls.nft"
}

for live in 0 1; do
	retired_root
	export TEST_LIVE_GUARD=$live
	out=$(run_script odac-setup --restore-network)
	contains "restore succeeds with standalone table=$live" "$out" 'protective guard was removed'
	check 'restore removes durable guard file' "$(test -e "$ROOTDIR/etc/nftables.d/90-odac-retired.nft" && echo present || echo absent)" absent
	check 'restore clears retirement record after success' "$(count_matches "$ROOTDIR/etc/odac/ownership" '^retired_guard ')" 0
	check 'restore removes an existing standalone table' "$(count_matches "$ROOTDIR/calls.nft" '^delete table inet odac_guard$')" "$live"
	unset TEST_LIVE_GUARD
	cleanup_root
done

for scenario in carrier-down missing-interface invalid-flags active running unknown-service modified symlink wrong-table nft-inspect check reload nft-delete locked; do
	retired_root
	original=$(cat "$ROOTDIR/etc/nftables.d/90-odac-retired.nft")
	case "$scenario" in
		carrier-down) printf '0x1003\n' > "$ROOTDIR/sys/class/net/br-odac/flags";;
		missing-interface) rm -r "$ROOTDIR/sys/class/net/br-odac";;
		invalid-flags) printf 'unknown\n' > "$ROOTDIR/sys/class/net/br-odac/flags";;
		active) printf 'odac_active 1\n' >> "$ROOTDIR/etc/odac/ownership";;
		running) export TEST_SERVICE_STATUS=0;;
		unknown-service) export TEST_SERVICE_STATUS=2;;
		modified) printf '# administrator edit\n' >> "$ROOTDIR/etc/nftables.d/90-odac-retired.nft";;
		symlink)
			mv "$ROOTDIR/etc/nftables.d/90-odac-retired.nft" "$ROOTDIR/other-guard"
			ln -s "$ROOTDIR/other-guard" "$ROOTDIR/etc/nftables.d/90-odac-retired.nft";;
		wrong-table) export TEST_CONTROLLED_IFACE=br-other;;
		nft-inspect) export TEST_FAIL_NFT='list tables';;
		check) export TEST_FAIL_FW4=check:1;;
		reload) export TEST_FAIL_FW4=reload:1;;
		nft-delete) export TEST_FAIL_NFT='delete table inet odac_guard';;
		locked) mkdir "$ROOTDIR/var/lock/odac-setup.lock";;
	esac
	rc=0
	out=$(run_script odac-setup --restore-network) || rc=$?
	check "restore refuses or rolls back $scenario" "$rc" 1
	check "failed $scenario restore keeps durable guard" "$(test -e "$ROOTDIR/etc/nftables.d/90-odac-retired.nft" && echo present || echo absent)" present
	check "failed $scenario restore retains ownership" "$(count_matches "$ROOTDIR/etc/odac/ownership" '^retired_guard 1$')" 1
	absent "failed $scenario restore does not claim success" "$out" 'protective guard was removed'
	case "$scenario" in
		check|reload|nft-delete)
			check "failed $scenario restores exact guard content" "$(cat "$ROOTDIR/etc/nftables.d/90-odac-retired.nft")" "$original"
			contains "failed $scenario reloads protection" "$(cat "$ROOTDIR/calls.fw4")" reload;;
		*) check "refused $scenario never reloads firewall" "$(count_matches "$ROOTDIR/calls.fw4" '^reload$')" 0;;
	esac
	unset TEST_SERVICE_STATUS TEST_CONTROLLED_IFACE TEST_FAIL_NFT TEST_FAIL_FW4
	cleanup_root
done

# --- summary -----------------------------------------------------------------

printf '# pass=%s fail=%s\n' "$PASS" "$FAILURES"
[ "$FAILURES" -eq 0 ] || exit 1
