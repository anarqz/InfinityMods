#!/usr/bin/env python3
"""Host tests of EasyRadio's core C (ini, ID3, MP3 duration incl. VBR, HUD text, RadioInfo edit, shuffle, logo,
generator), compiled for this host with Zig's clang (Linux or macOS) and run. Synthetic data only.

    python3 -I mods/easyradio/tests/run_tests.py --zig /path/to/zig [--exe /path/to/forza_durango_release_final.exe]

--exe also checks every `prologue_<name>[] = {...}; // rva 0x...` line of src/easyradio.c, and the two vtable words
the plugin relies on, against the player's own game executable (Azimuth 1.0.4's FH2); nothing of it is kept.
"""
import argparse
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parent / "src"
CORE = ["er_util.c", "er_text.c", "er_mp3.c", "er_ini.c", "er_xml.c", "er_logo.c", "er_gen.c", "er_track.c"]
PROLOGUE = re.compile(r"static const unsigned char prologue_(\w+)\[\] = \{([^}]*)\}; // rva (0x[0-9a-fA-F]+)")
IMAGE_BASE = 0x140000000
VTABLE_WORDS = (("RadioSystem vtable +0x08 = Load", 0x2e76820, 0x100fd20),
                ("RadioStreamFmod vtable +0x58 = Seek", 0x2e76220 + 0x58, 0x1021150))


def sections_of(data):
    if data[:2] != b"MZ":
        raise ValueError("not an MZ file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("no PE header")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    table = pe + 24 + optional_size
    return [struct.unpack_from("<IIII", data, table + 40 * i + 8) for i in range(count)]


def offset_of(sections, rva):
    for vsize, va, rsize, raw in sections:
        if va <= rva < va + max(vsize, rsize):
            return raw + rva - va if rva - va < rsize else None
    return None


def check_game(exe):
    data = exe.read_bytes()
    sections = sections_of(data)
    good = True
    found = PROLOGUE.findall((SRC / "easyradio.c").read_text(encoding="utf-8"))
    for name, body, rva_text in found:
        expected = bytes(int(b, 16) for b in body.replace(" ", "").split(",") if b)
        at = offset_of(sections, int(rva_text, 16))
        same = at is not None and data[at:at + len(expected)] == expected
        good &= same
        print(f"{'ok  ' if same else 'FAIL'} code {name} at rva {rva_text}")
    for name, rva, want in VTABLE_WORDS:
        at = offset_of(sections, rva)
        same = at is not None and struct.unpack_from("<Q", data, at)[0] == IMAGE_BASE + want
        good &= same
        print(f"{'ok  ' if same else 'FAIL'} data {name}")
    return good and len(found) >= 10


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--zig", type=Path, default=None, help="zig 0.15.2 (default: zig on PATH)")
    parser.add_argument("--exe", type=Path, default=None, help="the game executable, to check the code the plugin hooks")
    args = parser.parse_args()
    if sys.platform == "win32":
        sys.exit("the host tests use the POSIX file layer (tests/er_platform_posix.c): run them on Linux or macOS")
    zig = str(args.zig.resolve()) if args.zig else shutil.which("zig") or sys.exit("zig not found (--zig)")
    good = check_game(args.exe) if args.exe else True
    with tempfile.TemporaryDirectory(prefix="easyradio-tests-") as temporary:
        exe = Path(temporary) / "test_core"
        subprocess.run([zig, "cc", "-O2", "-Wall", "-Wextra", "-Werror", "-g", f"-DER_SEED_INI=\"{HERE.parent / 'easyradio.ini'}\"", "-o", str(exe), str(HERE / "test_core.c"),
                        *(str(SRC / c) for c in CORE), str(HERE / "er_platform_posix.c")], check=True)
        good &= subprocess.run([str(exe)]).returncode == 0
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
