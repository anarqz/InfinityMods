"""Host tests of Underglow's Infinity patches: the pinned upstream commit (git archive, LF) with the recipe's patches
applied, as tools/infmods.py build_mod does, then each *_test.cpp here compiled against its src/ with Zig's clang
(any OS) and run.

    python3 -I mods/underglow/tests/run_tests.py --zig /path/to/zig
"""
import argparse
import io
import json
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--zig", type=Path, default=None, help="zig 0.15.2 (default: zig on PATH)")
    args = parser.parse_args()
    zig = str(args.zig.resolve()) if args.zig else shutil.which("zig") or sys.exit("zig not found (--zig)")
    git = shutil.which("git") or sys.exit("git not found")
    recipe = json.loads((HERE.parent / "mod.json").read_text(encoding="utf-8"))
    upstream = ROOT / recipe["upstream"]["submodule"]
    failed = 0
    with tempfile.TemporaryDirectory(prefix="underglow-tests-") as temporary:
        tree = Path(temporary) / "src"
        tree.mkdir()
        archive = subprocess.run([git, "-c", "core.autocrlf=false", "-c", "core.eol=lf", "-C", str(upstream), "archive",
                                  "--format=tar", "HEAD"], check=True, capture_output=True).stdout
        with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
            tar.extractall(tree, filter="data")
        for patch in recipe["modBuild"].get("patches", []):
            subprocess.run([git, "apply", "--whitespace=nowarn", "-p1", str(ROOT / patch)], check=True, cwd=tree)
        for test in sorted(HERE.glob("*_test.cpp")):
            exe = Path(temporary) / (test.stem + (".exe" if sys.platform == "win32" else ""))
            subprocess.run([zig, "c++", "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(tree / "src"),
                            str(test), "-o", str(exe)], check=True)
            result = subprocess.run([str(exe)])
            print(f"{test.name}: {'ok' if result.returncode == 0 else 'FAILED'}")
            failed += result.returncode != 0
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
