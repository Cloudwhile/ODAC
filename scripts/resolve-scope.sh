#!/bin/sh
# Resolve the CI build scope into a GitHub Actions matrix.
#
# Usage: resolve-scope.sh EVENT_NAME REF_TYPE [DISPATCH_SCOPE]
#
# Prints two lines:
#   scope=<quick|full>
#   matrix={"include":[{...},{...}]}
#
# The architecture list is scripts/targets.txt. The same file is read by the
# release completeness check and by the workflow validation, so the three
# cannot drift apart.
#
# The matrix is a real selection: a quick run yields exactly one architecture,
# so no runner is allocated, no SDK is downloaded and no build is performed
# for the other targets. A step that only printed "skipping" would still pay
# all of those costs.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
event_name=${1:-}
ref_type=${2:-}
dispatch_scope=${3:-}

[ -n "$event_name" ] || { echo 'Usage: resolve-scope.sh EVENT_NAME REF_TYPE [DISPATCH_SCOPE]' >&2; exit 2; }

all_targets=$(grep -vE '^\s*(#|$)' "$here/targets.txt")
[ -n "$all_targets" ] || { echo 'scripts/targets.txt lists no target' >&2; exit 1; }

case "$event_name" in
	workflow_dispatch) scope=${dispatch_scope:-quick} ;;
	push)
		if [ "$ref_type" = tag ]; then scope=full; else scope=quick; fi ;;
	pull_request) scope=quick ;;
	*) scope=quick ;;
esac

case "$scope" in
	quick|full) :;;
	*) echo "unknown build scope: $scope" >&2; exit 2;;
esac

entries=''
count=0
while read -r target subtarget arch; do
	[ -n "$target" ] || continue
	if [ "$scope" = quick ] && [ "$target" != x86 ]; then
		continue
	fi
	count=$((count + 1))
	entries="${entries}{\"target\":\"${target}\",\"subtarget\":\"${subtarget}\",\"arch\":\"${arch}\"},"
done <<EOF
$all_targets
EOF
entries=${entries%,}
[ "$count" -gt 0 ] || { echo 'the build scope selected no architecture' >&2; exit 1; }

printf 'scope=%s\n' "$scope"
printf 'matrix={"include":[%s]}\n' "$entries"
printf 'count=%s\n' "$count"
