"""Host tests of the FH2 Mod Platform adapter's translations (API 10): tests/l10n_test.cpp, the adapter's
fh2_mod_platform_eo_l10n.cpp and each mod's own platform/src/l10n.cpp (its submodule's platform, as the adapter
build compiles it), against tests/shim/windows.h, with the mod's built-in translations. Any OS but Windows'
MSVC: a C++20 compiler (g++, clang++, or zig c++).

    python3 -I adapters/fh2-mod-platform-eo/tests/run_tests.py [--cxx "zig c++"]
"""
import argparse
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ADAPTER = HERE.parent
ROOT = ADAPTER.parent.parent

# submodule, its built-in translations (po2inc.py's .inc and its array), and one entry of its pt_BR.po
CASES = [
    ("upstream/nitrosystem", "src/nitro_l10n.inc", "nitro_translations", "Nitro System",
     "row.NitroEnabled.label", "Nitro System ({button})", "Sistema de Nitro ({button})"),
    ("upstream/underglow", "src/underglow_l10n.inc", "underglow_translations", "Underglow",
     "color.Cyan", "Cyan", "Ciano"),
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default=None, help="the C++ compiler (default: g++, else clang++)")
    args = parser.parse_args()
    cxx = shlex.split(args.cxx) if args.cxx else [shutil.which("g++") or shutil.which("clang++") or sys.exit("no C++ compiler (--cxx)")]
    failed = 0
    with tempfile.TemporaryDirectory(prefix="fh2mp-eo-tests-") as temporary:
        for submodule, inc, array, name, key, english, brazilian in CASES:
            upstream = ROOT / submodule
            platform = upstream / "platform"
            exe = Path(temporary) / (upstream.name + "_l10n_test")
            subprocess.run([*cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-DWIN32_LEAN_AND_MEAN", "-DNOMINMAX",
                            "-I", str(HERE / "shim"), "-I", str(platform / "include"), "-I", str(platform / "src"),
                            "-I", str(ADAPTER), f'-DTRANSLATIONS_INC="{upstream / inc}"', f"-DTRANSLATIONS={array}",
                            str(platform / "src" / "l10n.cpp"), str(ADAPTER / "fh2_mod_platform_eo_l10n.cpp"),
                            str(HERE / "l10n_test.cpp"), "-o", str(exe)], check=True)
            folder = Path(temporary) / (upstream.name + "-mod")
            folder.mkdir()
            result = subprocess.run([str(exe), str(folder), name, key, english, brazilian],
                                    env={"TEST_LOCALE": "pt-BR", "PATH": "/usr/bin:/bin"})
            print(f"{upstream.name}: {'ok' if result.returncode == 0 else 'FAILED'}")
            failed += result.returncode != 0
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
