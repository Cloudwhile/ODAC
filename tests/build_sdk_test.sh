#!/bin/sh
# Exercise SDK validation through the build entry point without downloading feeds.
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
sdk="$work/sdk"
mkdir -p "$sdk/include" "$sdk/scripts" "$sdk/package"
: > "$sdk/include/package-pack.mk"
cat > "$sdk/scripts/feeds" <<'EOF'
#!/bin/sh
# Reaching this stub proves validation succeeded; stop before any build work.
exit 42
EOF
chmod +x "$sdk/scripts/feeds"

check() {
    label=$1; configured=$2; fallback=$3; target=$4; expected=$5
    rm -rf "$sdk/package/odac"
    printf 'CONFIG_TARGET_BOARD="x86"\nCONFIG_TARGET_SUBTARGET="64"\n' > "$sdk/.config"
    if [ "$configured" != absent ]; then
        printf 'CONFIG_VERSION_NUMBER="%s"\n' "$configured" >> "$sdk/.config"
    fi
    printf 'VERSION_NUMBER:=$(if $(VERSION_NUMBER),$(VERSION_NUMBER),%s)\n' "$fallback" > "$sdk/include/version.mk"
    rc=0
    sh "$project/scripts/build-sdk.sh" "$sdk" "$target" 64 > "$work/output" 2>&1 || rc=$?
    if [ "$rc" -ne "$expected" ]; then
        cat "$work/output"
        printf 'FAIL: %s (expected %s, got %s)\n' "$label" "$expected" "$rc" >&2
        exit 1
    fi
    printf 'PASS: %s\n' "$label"
}

check 'missing configured version uses SDK default' absent 25.12.2 x86 42
check 'empty configured version uses SDK default' '' 25.12.2 x86 42
check 'nonempty configured version takes precedence' 25.12.3 24.10.0 x86 42
check 'unsupported configured version is rejected' 24.10.0 25.12.2 x86 1
check 'unsupported fallback is rejected' '' 24.10.0 x86 1
check 'missing release is rejected' absent '' x86 1
check 'target mismatch is rejected' '' 25.12.2 mediatek 1
