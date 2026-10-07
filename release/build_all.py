"""Everything for a BlockBreach release, in order, stopping at the first failure:

  python release/build_all.py

  1. the RoN host mod + UE4SS (scripts/build-host.bat)        2. the Minecraft mod (Gradle)
  3. the installer's payload (installer/make_payload.py)      4. the installer (cargo build --release)
  5. the installer's self-test (fake game folders)            6. dist/ (release/make_release.py)
  7. no build-machine paths in the release (release/scan_paths.py)
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def step(name, cmd, cwd=ROOT):
    print(f"== {name}", flush=True)
    r = subprocess.run(cmd, cwd=cwd, shell=isinstance(cmd, str))
    if r.returncode != 0:
        sys.exit(f"FAILED: {name} (exit {r.returncode})")


def main():
    step("host mod", ["cmd", "/c", str(ROOT / "scripts" / "build-host.bat")])
    gradlew = ROOT / "src" / "mc-gta5-passthrough-example" / "mc" / "gradlew.bat"
    step("minecraft mod", [str(gradlew), "build", "-q"], cwd=gradlew.parent)
    step("payload", [sys.executable, str(ROOT / "installer" / "make_payload.py")])
    step("installer", ["cargo", "build", "--release"], cwd=ROOT / "installer")
    with tempfile.TemporaryDirectory() as tmp:
        step("self-test", [str(ROOT / "installer" / "target" / "release" / "BlockBreach.exe"), "--selftest", tmp])
        report = (Path(tmp) / "selftest.txt").read_text()
        print(report)
        if "ALL PASSED" not in report:
            sys.exit("FAILED: self-test")
    step("release files", [sys.executable, str(ROOT / "release" / "make_release.py")])
    dist = ROOT / "dist"
    files = [str(p) for p in dist.glob("BlockBreach-*/BlockBreach.exe")] + [str(p) for p in dist.glob("*.zip")]
    r = subprocess.run([sys.executable, str(ROOT / "release" / "scan_paths.py"), *files], capture_output=True, text=True)
    print(r.stdout)
    if "clean" not in r.stdout:
        sys.exit("FAILED: build-machine paths in the release")
    print("release ready in", dist)


if __name__ == "__main__":
    main()
