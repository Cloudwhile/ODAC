#!/usr/bin/env python3
"""Check ODAC workflow contracts and shell syntax, without running the SDK.

This is a targeted regression check, not a general shell dataflow analyzer or
an assertion that GitHub Actions, packaging, or router acceptance has passed.
"""
from pathlib import Path
import re
import subprocess
import sys

import yaml


def check(workflow, rows):
    errors = []
    jobs = workflow.get("jobs", {})
    for required in ("scope", "fast-check", "build", "release"):
        if required not in jobs:
            errors.append(f"missing job: {required}")
    for name, job in jobs.items():
        needs = job.get("needs", [])
        if isinstance(needs, str):
            needs = [needs]
        if any(need not in jobs for need in needs):
            errors.append(f"unknown job dependency in {name}")
        checked_out = False
        output_producers = {}
        for step in job.get("steps", []):
            uses = step.get("uses", "")
            body = step.get("run", "")
            if uses.startswith("actions/checkout@"):
                checked_out = True
            if "scripts/" in body and not checked_out:
                errors.append(f"{name} uses repository scripts before checkout")
            syntax = subprocess.run(["bash", "-n"], input=body, text=True,
                                    capture_output=True)
            if syntax.returncode:
                errors.append(f"shell syntax in {name}/{step.get('name')}: {syntax.stderr.strip()}")
            # Action inputs are not shell scripts: ${TARGET} stays literal.
            inputs = step.get("with", {})
            if re.search(r"\$\{(?!\{)", yaml.safe_dump(inputs)):
                errors.append(f"{name}: shell expansion used in action inputs")
            for source, output in re.findall(r"steps\.([\w-]+)\.outputs\.([\w-]+)",
                                             yaml.safe_dump(step)):
                if source in output_producers and output_producers[source] is not None:
                    if output not in output_producers[source]:
                        errors.append(f"undeclared output: steps.{source}.outputs.{output}")
            if step.get("id"):
                output_producers[step["id"]] = None if uses else set(re.findall(
                    r'echo\s+"([A-Za-z_][A-Za-z0-9_]*)=.*?"\s*>>\s*"\$GITHUB_OUTPUT"', body))
            if uses.startswith("actions/cache/"):
                key = inputs.get("key", "")
                for field in ("target", "subtarget", "arch"):
                    if "${{ matrix." + field + " }}" not in key:
                        errors.append(f"cache key lacks matrix.{field}")
                if key.startswith("dl-") and inputs.get("path") != "${{ steps.config.outputs.sdk_dir }}/dl":
                    errors.append("source cache does not use the extracted SDK dl directory")

    build = jobs.get("build", {})
    matrix = build.get("strategy", {}).get("matrix")
    if matrix != "${{ fromJSON(needs.scope.outputs.matrix) }}":
        errors.append("build must use the scope output matrix")
    if not {"scope", "fast-check"}.issubset(set(build.get("needs", []))):
        errors.append("SDK build must depend on scope and fast-check")
    scope = jobs.get("scope", {})
    if "matrix" not in scope.get("outputs", {}):
        errors.append("scope does not publish its matrix")

    bodies = {step.get("id"): step.get("run", "") for step in build.get("steps", [])}
    sdk = bodies.get("sdk", "")
    if "scripts/verify-sdk.py" not in sdk or '--archive "sdk-cache/${archive}"' not in sdk:
        errors.append("SDK verification must validate the selected archive")
    collect = bodies.get("collect", "")
    if 'package_dir="${SDK_DIR}/bin"' not in collect:
        errors.append("package discovery must cover both feed and target output directories")
    if "expected=noarch" not in collect:
        errors.append("architecture independent APKs must declare noarch")
    if '"${apk_bin}" adbdump' not in collect:
        errors.append("collection must read actual APK metadata")
    if '"${SDK_DIR}" "${TARGET}" "${SUBTARGET}"' not in bodies.get("build", ""):
        errors.append("build must receive SDK_DIR, TARGET and SUBTARGET")

    release = jobs.get("release", {})
    condition = release.get("if", "")
    if not all(token in condition for token in ("github.event_name == 'push'", "refs/tags/v", "needs.scope.outputs.scope == 'full'")):
        errors.append("release requires a version tag push and a full scope")
    if workflow.get("permissions", {}).get("contents") != "read":
        errors.append("workflow default permission must be contents: read")
    for name, job in jobs.items():
        if name != "release" and job.get("permissions", {}).get("contents") == "write":
            errors.append(f"{name} must not have release write permission")
    if release.get("permissions", {}).get("contents") != "write":
        errors.append("release needs contents: write")
    release_code = "\n".join(step.get("run", "") for step in release.get("steps", []))
    for token in ("--draft --title", "gh release download", "cmp out/SHA256SUMS uploaded/SHA256SUMS", "--draft=false"):
        if token not in release_code:
            errors.append(f"release lacks draft verification: {token}")
    if "--clobber" in release_code:
        errors.append("release must not overwrite assets")

    if any(len(row) != 3 for row in rows):
        errors.append("malformed target row")
    pairs = [(row[0], row[1]) for row in rows if len(row) == 3]
    if len(pairs) != len(set(pairs)):
        errors.append("duplicate target/subtarget pair")
    if len(rows) != 11 or ("x86", "64") not in pairs or ("mediatek", "filogic") not in pairs:
        errors.append("the 11-target matrix must include x86/64 and mediatek/filogic")
    return errors


def main():
    workflow = yaml.safe_load(Path(sys.argv[1]).read_text(encoding="utf-8"))
    rows = [line.split() for line in Path(sys.argv[2]).read_text().splitlines()
            if line.strip() and not line.lstrip().startswith("#")]
    errors = check(workflow, rows)
    for error in errors:
        print(f"FAIL: {error}", file=sys.stderr)
    if errors:
        return 1
    print(f"PASS: workflow contracts and shell syntax; {len(rows)} targets")
    return 0


if __name__ == "__main__":
    sys.exit(main())
