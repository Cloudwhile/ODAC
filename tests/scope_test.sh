#!/bin/sh
# Test the CI build scope resolution.
#
# The scope decides which architectures are built. A quick run that still
# built every architecture would waste the whole point of the change, so the
# matrix content is checked, not just the scope label.
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
scope_script="$project/scripts/resolve-scope.sh"

PASS=0
FAILURES=0
ok() { PASS=$((PASS + 1)); printf 'PASS: %s\n' "$1"; }
not_ok() { FAILURES=$((FAILURES + 1)); printf 'FAIL: %s\n' "$1" >&2; }

resolve() {
	sh "$scope_script" "$@"
}

# --- the scope label ---------------------------------------------------------
for trigger in "pull_request branch" "push branch" "push tag" "workflow_dispatch branch"; do
	# shellcheck disable=SC2086
	set -- $trigger
	result=$(resolve "$1" "$2" quick)
	scope=$(printf '%s\n' "$result" | sed -n 's/^scope=//p')
	case "$1" in
		push) expected=quick; [ "$2" = tag ] && expected=full ;;
		workflow_dispatch) expected=quick ;;
		*) expected=quick ;;
	esac
	if [ "$scope" = "$expected" ]; then
		ok "the scope of $trigger is $expected"
	else
		not_ok "the scope of $trigger is $expected (got '$scope')"
	fi
done

# A manual full dispatch selects the whole matrix.
result=$(resolve workflow_dispatch branch full)
scope=$(printf '%s\n' "$result" | sed -n 's/^scope=//p')
count=$(printf '%s\n' "$result" | sed -n 's/^count=//p')
total=$(grep -cvE '^\s*(#|$)' "$project/scripts/targets.txt")
if [ "$scope" = full ] && [ "$count" = "$total" ]; then
	ok "a manual full dispatch selects all $total architectures"
else
	not_ok "a manual full dispatch selects all $total architectures (scope=$scope count=$count)"
fi

# --- the matrix really is restricted ------------------------------------------
# A quick run must produce a matrix with exactly one entry. A matrix that
# still listed every architecture would allocate a runner and download an
# SDK for each of them.
result=$(resolve pull_request branch quick)
matrix=$(printf '%s\n' "$result" | sed -n 's/^matrix=//p')
entries=$(printf '%s' "$matrix" | tr '{' '\n' | grep -c '"target"')
if [ "$entries" -eq 1 ]; then
	ok 'a pull request matrix contains exactly one architecture'
else
	not_ok "a pull request matrix contains exactly one architecture (found $entries)"
fi
if printf '%s' "$matrix" | grep -q '"target":"x86"'; then
	ok 'a quick matrix contains x86'
else
	not_ok 'a quick matrix contains x86'
fi
if printf '%s' "$matrix" | grep -q '"target":"mediatek"'; then
	not_ok 'a quick matrix does not contain mediatek'
else
	ok 'a quick matrix does not contain mediatek'
fi

# The full matrix must contain every target in the list, including the two
# that share the aarch64_cortex-a53 architecture.
result=$(resolve push tag full)
matrix=$(printf '%s\n' "$result" | sed -n 's/^matrix=//p')
missing=''
while read -r target subtarget arch; do
	[ -n "$target" ] || continue
	printf '%s' "$matrix" | grep -q "\"target\":\"${target}\",\"subtarget\":\"${subtarget}\"" ||
		missing="$missing ${target}/${subtarget}"
done <<EOF
$(grep -vE '^\s*(#|$)' "$project/scripts/targets.txt")
EOF
if [ -z "$missing" ]; then
	ok 'the full matrix contains every supported target'
else
	not_ok "the full matrix is missing:$missing"
fi

# Two targets share the same architecture. Their artifacts must still be
# distinguishable, which is why the file names carry the target.
a53=$(grep -c 'aarch64_cortex-a53' "$project/scripts/targets.txt")
if [ "$a53" -ge 2 ]; then
	ok 'the shared aarch64_cortex-a53 architecture is used by more than one target'
else
	not_ok 'the shared aarch64_cortex-a53 architecture is used by more than one target'
fi

# --- an unknown scope is rejected --------------------------------------------
# The scope is taken from a workflow input, so an unexpected value must stop
# the run instead of silently falling back to a partial build.
if sh "$scope_script" workflow_dispatch branch nonsense >/dev/null 2>&1; then
	not_ok 'an unknown scope is rejected'
else
	ok 'an unknown scope is rejected'
fi

# A missing event name is a usage error, not a default.
if sh "$scope_script" >/dev/null 2>&1; then
	not_ok 'a missing event name is rejected'
else
	ok 'a missing event name is rejected'
fi

printf '\n# pass=%s fail=%s\n' "$PASS" "$FAILURES"
[ "$FAILURES" -eq 0 ] || exit 1
