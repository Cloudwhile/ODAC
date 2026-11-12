#!/usr/bin/env python3
"""Exercise CI rejection paths with local fixtures; never run downloaded code."""
import copy
import hashlib
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[1]
CHECK = runpy.run_path(str(ROOT / "tests/workflow_check.py"))["check"]
WORKFLOW = yaml.safe_load((ROOT / ".github/workflows/build-apk.yml").read_text())
ROWS = [line.split() for line in (ROOT / "scripts/targets.txt").read_text().splitlines()
        if line.strip() and not line.lstrip().startswith("#")]


class WorkflowTests(unittest.TestCase):
    def test_baseline(self):
        self.assertEqual(CHECK(WORKFLOW, ROWS), [])

    def test_regressions_are_rejected_for_the_expected_reason(self):
        def no_checkout(w):
            w["jobs"]["scope"]["steps"].pop(0)

        def literal_cache(w):
            for step in w["jobs"]["build"]["steps"]:
                if step.get("id") == "cache_sdk":
                    step["with"]["key"] = "sdk-${TARGET}-${SUBTARGET}-${ARCH}"

        def wrong_cache_path(w):
            for step in w["jobs"]["build"]["steps"]:
                if step.get("id") == "cache_dl":
                    step["with"]["path"] = "sdk/dl"

        def change_step(w, step_id, old, new):
            step = next(s for s in w["jobs"]["build"]["steps"] if s.get("id") == step_id)
            self.assertIn(old, step["run"])
            step["run"] = step["run"].replace(old, new)

        def undeclared_output(w):
            next(s for s in w["jobs"]["build"]["steps"] if s.get("id") == "build")["env"] = {
                "SDK_DIR": "${{ steps.config.outputs.SDK_DIR }}"}

        cases = [
            (no_checkout, "before checkout"),
            (literal_cache, "shell expansion used in action inputs"),
            (wrong_cache_path, "source cache"),
            (undeclared_output, "undeclared output"),
            (lambda w: change_step(w, "sdk", "--archive", "--unused"), "SDK verification"),
            (lambda w: change_step(w, "collect", '${SDK_DIR}/bin"', '${SDK_DIR}/bin/packages"'), "package discovery"),
            (lambda w: change_step(w, "collect", "expected=noarch", "expected=all"), "declare noarch"),
            (lambda w: change_step(w, "collect", "python3 -I scripts/check-apk-content.py", "true"), "check-apk-content.py"),
            (lambda w: change_step(w, "collect", "python3 -I scripts/check-apk-elf.py", "true"), "check-apk-elf.py"),
            (lambda w: change_step(w, "build", '"${SUBTARGET}"', '"${NOT_DEFINED}"'), "build must receive"),
        ]
        for mutate, expected in cases:
            with self.subTest(expected=expected):
                variant = copy.deepcopy(WORKFLOW)
                mutate(variant)
                self.assertNotEqual(variant, WORKFLOW)
                errors = CHECK(variant, ROWS)
                self.assertTrue(any(expected in error for error in errors), errors)

    def test_collection_checks_each_packages_own_version(self):
        collect = next(step["run"] for step in WORKFLOW["jobs"]["build"]["steps"]
                       if step.get("id") == "collect")
        # Run the actual version gate, independent of SDK discovery and payload checks.
        gate = collect[collect.index('case "$pkg" in'):collect.index('echo "${pkg}: arch=')]
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            for package, recipe, version, release in [
                ("libargon2", "argon2", "20190702", "3"),
                ("odac-opennds", "odac-opennds", "10.3.1", "4"),
                ("device-access", "device-access", "0.1.0_rc1", "2"),
                ("luci-app-device-access", "luci-app-device-access", "0.2.0", "7"),
            ]:
                (folder / recipe).mkdir()
                (folder / recipe / "Makefile").write_text(
                    f"PKG_VERSION:={version}\nPKG_RELEASE:={release}\n")
                for declared, ok in [(f"{version}-r{release}", True), ("0.1.0_rc1-r99", False)]:
                    with self.subTest(package=package, declared=declared):
                        result = subprocess.run(
                            ["bash", "-eu", "-c", gate], cwd=folder,
                            env={**os.environ, "pkg": package, "version": declared},
                            capture_output=True, text=True)
                        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
                        if not ok:
                            self.assertIn("declares version", result.stdout)

    def test_release_accepts_independent_luci_version_and_rejects_incomplete_set(self):
        gate = next(step["run"] for step in WORKFLOW["jobs"]["release"]["steps"]
                    if step.get("name") == "Verify the attachment set before publishing")
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / "scripts").mkdir()
            (folder / "scripts/targets.txt").write_text("x86 64 x86_64\n")
            artifact = folder / "dist/odac-apk-x86-64"
            artifact.mkdir(parents=True)
            entries = [
                ("libargon2", "argon2", "20190702", "3"),
                ("odac-opennds", "odac-opennds", "10.3.1", "4"),
                ("device-access", "device-access", "0.1.0_rc1", "2"),
                ("luci-app-device-access", "luci-app-device-access", "0.2.0", "7"),
            ]
            manifest = []
            for package, recipe, version, release in entries:
                (folder / recipe).mkdir()
                (folder / recipe / "Makefile").write_text(
                    f"PKG_VERSION:={version}\nPKG_RELEASE:={release}\n")
                name = f"{package}-{version}-r{release}-x86-64.apk"
                payload = package.encode()
                (artifact / name).write_bytes(payload)
                manifest.append(f"{hashlib.sha256(payload).hexdigest()}  ./{name}\n")
            (artifact / "SHA256SUMS").write_text("".join(manifest))
            result = subprocess.run(["bash", "-c", gate], cwd=folder,
                                    env={**os.environ, "SCOPE": "full"},
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            # Remove LuCI and make the remaining manifest valid: completeness must still fail.
            for path in (folder / "out").iterdir():
                path.unlink()
            (artifact / name).unlink()
            (artifact / "SHA256SUMS").write_text("".join(manifest[:-1]))
            result = subprocess.run(["bash", "-c", gate], cwd=folder,
                                    env={**os.environ, "SCOPE": "full"},
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(f"missing {name}", result.stdout)

    def test_scope_json_and_exit_status(self):
        import json
        for event, ref, requested, count in [
            ("pull_request", "branch", "full", 1),
            ("push", "branch", "full", 1),
            ("push", "tag", "quick", 11),
            ("workflow_dispatch", "branch", "quick", 1),
            ("workflow_dispatch", "branch", "full", 11),
        ]:
            with self.subTest(event=event, ref=ref, requested=requested):
                result = subprocess.run(["sh", str(ROOT / "scripts/resolve-scope.sh"), event, ref, requested],
                                        text=True, capture_output=True, check=True)
                fields = dict(line.split("=", 1) for line in result.stdout.splitlines())
                entries = json.loads(fields["matrix"])["include"]
                self.assertEqual(int(fields["count"]), count)
                expected = ROWS if count == 11 else [["x86", "64", "x86_64"]]
                self.assertEqual(entries, [dict(zip(("target", "subtarget", "arch"), row)) for row in expected])


class ChecksumTests(unittest.TestCase):
    def test_manifest_and_archive_rejection_paths(self):
        name = "openwrt-sdk-25.12.2-x86-64_gcc-14.3.0_musl.Linux-x86_64.tar.zst"
        payload = b"local archive fixture, not an SDK"
        digest = hashlib.sha256(payload).hexdigest()
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            archive = folder / name
            manifest = folder / "sha256sums"
            archive.write_bytes(payload)
            command = ["python3", str(ROOT / "scripts/verify-sdk.py"), str(manifest), "25.12.2", "x86", "64"]
            for label, contents, ok in [
                ("binary marker", f"{digest} *{name}\n", True),
                ("text marker", f"{digest}  {name}\n", True),
                ("empty", "", False),
                ("wrong name", f"{digest} *{name}.suffix\n", False),
                ("duplicate", f"{digest} *{name}\n" * 2, False),
                ("bad digest", f"not-a-sha *{name}\n", False),
                ("wrong target", f"{digest} *{name.replace('x86-64', 'mediatek-filogic')}\n", False),
            ]:
                with self.subTest(label=label):
                    manifest.write_text(contents)
                    result = subprocess.run(command + ["--archive", str(archive)], capture_output=True, text=True)
                    self.assertEqual(result.returncode == 0, ok, result.stderr)
            manifest.write_text(f"{digest} *{name}\n")
            archive.write_bytes(payload + b"corruption")
            result = subprocess.run(command + ["--archive", str(archive)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("SHA256 differs", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
