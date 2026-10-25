#!/usr/bin/env python3
"""Build, package and catalog the InfinityMods mods. Standard library only; run with `python3 -I`.

  build   <mod> --zig ZIG [--out DIR]       compile the mod and its adapter (Zig 0.15.2, any host)
  verify  <mod> --zig ZIG                   build twice in separate folders: same bytes, sane PE
  release --zig ZIG --tag TAG --catalog CURRENT.json [--out DIR] [--allow-dirty] [MOD ...]

`release` writes one ZIP per mod (<id>-<version>.zip), each mod's filled manifest and the merged
catalog. A mod whose ZIP has the same SHA-256 as its current catalog entry keeps that entry
unchanged (URL, version and source commit), so players see no update; a changed ZIP needs a new
version. Nothing is uploaded or published.
"""
import argparse
import copy
import hashlib
import io
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ZIG_VERSION = "0.15.2"
ZIP_TIME = (2026, 1, 1, 0, 0, 0)
CATALOG_FIELDS = ("id", "version", "name", "description", "author", "package", "additionalPackages")
RELEASE = json.loads((ROOT / "release.json").read_text(encoding="utf-8"))


def load_mod(name):
    folder = ROOT / "mods" / name
    recipe = json.loads((folder / "mod.json").read_text(encoding="utf-8"))
    manifest = json.loads((folder / "manifest.json").read_text(encoding="utf-8"))
    if recipe["id"] != name or manifest["id"] != name or manifest["version"] != recipe["version"]:
        sys.exit(f"{name}: mod.json and manifest.json disagree on id/version")
    return folder, recipe, manifest


def zig_check(zig):
    version = subprocess.run([str(zig), "version"], check=True, capture_output=True, text=True).stdout.strip()
    if version != ZIG_VERSION:
        sys.exit(f"Zig {ZIG_VERSION} required (official tarball layout), found {version}")
    if not (zig.parent / "lib/libc/include/any-windows-any").is_dir():
        sys.exit("Zig's lib/ is not next to the binary: use the official tarball, not a distro package")


def version_resource(zig, work, recipe):
    info = recipe["versionInfo"]
    numbers = recipe["version"].replace("-infinity.", ".").split(".")
    if len(numbers) != 4 or not all(n.isdigit() for n in numbers):
        sys.exit(f"{recipe['id']}: version must be <a.b.c>-infinity.<n>")
    strings = {"CompanyName": info["company"], "FileDescription": info["description"], "FileVersion": recipe["version"],
               "InternalName": recipe["dll"].rsplit(".", 1)[0], "LegalCopyright": info["license"],
               "OriginalFilename": recipe["dll"], "ProductName": info["product"], "ProductVersion": recipe["version"]}
    values = "\n".join(f'            VALUE "{k}", "{v}"' for k, v in strings.items())
    script = work / "version.rc"
    script.write_text(f"""1 VERSIONINFO
FILEVERSION {",".join(numbers)}
PRODUCTVERSION {",".join(numbers)}
FILEOS 0x40004
FILETYPE 0x2
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904B0"
        BEGIN
{values}
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
""", encoding="utf-8")
    resource = work / "version.res"
    subprocess.run([str(zig), "rc", "/fo", resource.name, "--", script.name], check=True, cwd=work)
    return resource


def build(name, zig, out):
    _, recipe, _ = load_mod(name)
    upstream = ROOT / recipe["upstream"]["submodule"]
    adapter = ROOT / "adapters" / recipe["adapter"]
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="infmods-") as temporary:
        work = Path(temporary)
        includes = [ROOT / "sdk", adapter, *(upstream / folder for folder in recipe["upstream"]["includes"])]
        # Upstream's own flags (build.py), so its sources compile exactly as they do there.
        flags = [str(zig), "clang", "-target", "x86_64-w64-windows-gnu", "-c", "-ffreestanding", "-fno-builtin",
                 "-fno-stack-protector", "-fms-extensions", "-funwind-tables", "-O2", "-Wall", "-Wextra", "-Werror",
                 *(a for folder in includes for a in ("-I", str(folder))),
                 "-isystem", str(zig.parent / "lib/include"),
                 "-isystem", str(zig.parent / "lib/libc/include/any-windows-any")]
        sources = [upstream / s for s in recipe["upstream"]["sources"]] + sorted(adapter.glob("*.c"))
        objects = []
        for index, source in enumerate(sources):
            obj = f"{index:02d}-{source.stem}.obj"  # relative: the PDB records these names, not the temp folder
            subprocess.run([*flags, str(source), "-o", obj], check=True, cwd=work)
            objects.append(obj)
        libraries = []
        for definition in recipe["upstream"]["importDefinitions"]:
            library = Path(definition).stem + ".lib"
            subprocess.run([str(zig), "dlltool", "-m", "i386:x86-64", "-d", str(upstream / definition), "-l", library],
                           check=True, cwd=work)
            libraries.append(library)
        dll = out / recipe["dll"]
        subprocess.run([str(zig), "lld-link", "/dll", "/entry:DllMain", "/machine:x64", "/nodefaultlib", "/dynamicbase",
                        "/nxcompat", "/Brepro", "/out:" + dll.name, "/implib:mod.lib", *objects, *libraries,
                        version_resource(zig, work, recipe).name, "/release", "/debug", "/pdb:" + dll.stem + ".pdb",
                        "/pdbaltpath:%_PDB%", "/pdbsourcepath:C:\\infinity-mods"], check=True, cwd=work)
        shutil.copyfile(work / dll.name, dll)
    return dll


def pe_report(path):
    """(machine, exports, imported DLLs) from a PE32+ file, stdlib only."""
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[:2] != b"MZ" or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    machine, sections, _, _, _, optional_size, _ = struct.unpack_from("<HHIIIHH", data, pe + 4)
    optional = pe + 24
    if struct.unpack_from("<H", data, optional)[0] != 0x20B:
        raise ValueError("not PE32+")
    directories = optional + 112
    table = [struct.unpack_from("<8sIIII", data, optional + optional_size + 40 * i) for i in range(sections)]

    def offset(rva):
        for _, size, address, raw_size, raw in table:
            if address <= rva < address + max(size, raw_size):
                return raw + rva - address
        raise ValueError(f"RVA {rva:#x} outside sections")

    def text(rva):
        start = offset(rva)
        return data[start:data.index(b"\0", start)].decode("ascii")

    exports, imports = [], []
    export_rva = struct.unpack_from("<I", data, directories)[0]
    if export_rva:
        e = offset(export_rva)
        count, names = struct.unpack_from("<I", data, e + 24)[0], struct.unpack_from("<I", data, e + 32)[0]
        exports = [text(struct.unpack_from("<I", data, offset(names) + 4 * i)[0]) for i in range(count)]
    import_rva = struct.unpack_from("<I", data, directories + 8)[0]
    if import_rva:
        i = offset(import_rva)
        while struct.unpack_from("<I", data, i + 12)[0]:
            imports.append(text(struct.unpack_from("<I", data, i + 12)[0]))
            i += 20
    return machine, exports, imports


def verify(name, zig):
    _, recipe, _ = load_mod(name)
    with tempfile.TemporaryDirectory(prefix="infmods-a-") as a, tempfile.TemporaryDirectory(prefix="infmods-b-") as b:
        first, second = build(name, zig, Path(a)), build(name, zig, Path(b))
        digests = [hashlib.sha256(p.read_bytes()).hexdigest() for p in (first, second)]
        machine, exports, imports = pe_report(first)
    problems = []
    if digests[0] != digests[1]:
        problems.append("two builds differ (not reproducible)")
    if machine != 0x8664:
        problems.append("not x64")
    if "InfinityModInitializeV1" not in exports:
        problems.append("InfinityModInitializeV1 not exported")
    if recipe["adapter"] == "fh2-modloader" and "FH2ModLoad" not in exports:
        problems.append("FH2ModLoad missing: the adapter has nothing to start")
    unexpected = [d for d in imports if d.lower() not in ("kernel32.dll", "ntdll.dll")]
    if unexpected:
        problems.append(f"unexpected imports {unexpected}")
    print(f"{recipe['dll']} sha256 {digests[0]}")
    print(f"exports {exports}\nimports {imports}")
    for p in problems:
        print("FAIL", p)
    return 1 if problems else 0


def package(folder, recipe, dll, out):
    """Deterministic ZIP: fixed time, sorted names, fixed permissions."""
    entries = {
        recipe["dll"]: dll.read_bytes(),
        "LICENSE.txt": (folder / "LICENSE").read_bytes(),
        "LICENSE-Infinity.txt": (ROOT / "adapters" / recipe["adapter"] / "LICENSE").read_bytes(),
        "NOTICE.md": (folder / "NOTICE.md").read_bytes(),
    }
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for name in sorted(entries):
            info = zipfile.ZipInfo(name, ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            info.create_system = 0
            archive.writestr(info, entries[name], compresslevel=9)
    path = out / f"{recipe['id']}-{recipe['version']}.zip"
    path.write_bytes(buffer.getvalue())
    return path, {n: (hashlib.sha256(b).hexdigest(), len(b)) for n, b in entries.items()}


def fill_manifest(manifest, recipe, zip_path, contents, url, commit):
    filled = copy.deepcopy(manifest)
    data = zip_path.read_bytes()
    filled["package"] = {"url": url, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data), "format": "zip"}
    mapped = set()
    for file in filled["files"]["azimuth"] + filled["files"]["game"]:
        if file["source"] not in contents:
            sys.exit(f"{recipe['id']}: manifest maps {file['source']}, which the package does not contain")
        file["sha256"], file["bytes"] = contents[file["source"]]
        mapped.add(file["source"])
    if mapped != set(contents):
        sys.exit(f"{recipe['id']}: package entries not mapped: {sorted(set(contents) - mapped)}")
    filled["source"]["commit"] = commit
    return filled


def catalog_entry(manifest):
    entry = {k: manifest[k] for k in ("id", "version", "name", "description", "author")}
    entry["download"] = {k: manifest["package"][k] for k in ("url", "sha256", "bytes")}
    entry["requires"] = ["infinity-mod-runtime"]
    entry["install"] = {k: v for k, v in manifest.items() if k not in CATALOG_FIELDS}
    return entry


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True, capture_output=True, text=True).stdout.strip()


def release(names, zig, tag, catalog_path, out, allow_dirty):
    if git("status", "--porcelain") and not allow_dirty:
        sys.exit("working tree not clean: the manifests pin the commit the packages are built from")
    commit = git("rev-parse", "HEAD")
    catalog = json.loads(Path(catalog_path).read_text(encoding="utf-8"))
    current = {m["id"]: m for m in catalog["mods"]}
    if out.exists():
        shutil.rmtree(out)
    (out / "assets").mkdir(parents=True)
    (out / "manifests").mkdir()
    report = []
    for name in names or sorted(p.name for p in (ROOT / "mods").iterdir() if (p / "mod.json").exists()):
        folder, recipe, manifest = load_mod(name)
        with tempfile.TemporaryDirectory(prefix="infmods-") as temporary:
            dll = build(name, zig, Path(temporary))
            zip_path, contents = package(folder, recipe, dll, out / "assets")
        digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
        existing = current.get(recipe["id"])
        if existing and existing.get("download", {}).get("sha256") == digest:
            report.append(f"{recipe['id']}: unchanged, keeps {existing['download']['url']}")
            continue  # the entry stays byte-for-byte; the asset is re-attached to this release only for completeness
        if existing and existing.get("version") == recipe["version"]:
            sys.exit(f"{recipe['id']}: package changed but version {recipe['version']} did not; bump it")
        url = f"{RELEASE['repository']}/releases/download/{tag}/{zip_path.name}"
        filled = fill_manifest(manifest, recipe, zip_path, contents, url, commit)
        (out / "manifests" / f"{recipe['id']}-{recipe['version']}.json").write_text(
            json.dumps(filled, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        entry = catalog_entry(filled)
        if existing:
            catalog["mods"][catalog["mods"].index(existing)] = entry
        else:
            catalog["mods"].append(entry)
        report.append(f"{recipe['id']}: {'updated' if existing else 'new'} {recipe['version']} at {url}")
    (out / "catalog.json").write_text(json.dumps(catalog, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    (out / "RELEASE.txt").write_text(f"tag {tag}\ncommit {commit}\n" + "\n".join(report) + "\n", encoding="utf-8")
    print("\n".join(report))
    return 0


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="cmd", required=True)
    for cmd in ("build", "verify"):
        p = sub.add_parser(cmd)
        p.add_argument("mod")
        p.add_argument("--zig", type=Path, required=True)
        if cmd == "build":
            p.add_argument("--out", type=Path, default=ROOT / "build")
    r = sub.add_parser("release")
    r.add_argument("mods", nargs="*")
    r.add_argument("--zig", type=Path, required=True)
    r.add_argument("--tag", required=True)
    r.add_argument("--catalog", required=True, help="the current official catalog.json (read only)")
    r.add_argument("--out", type=Path, default=ROOT / "dist")
    r.add_argument("--allow-dirty", action="store_true", help="development only: the commit pinned is HEAD, not the tree")
    args = parser.parse_args()
    zig = args.zig.resolve()
    zig_check(zig)
    if args.cmd == "build":
        print(build(args.mod, zig, args.out.resolve()))
        return 0
    if args.cmd == "verify":
        return verify(args.mod, zig)
    return release(args.mods, zig, args.tag, args.catalog, args.out.resolve(), args.allow_dirty)


if __name__ == "__main__":
    sys.exit(main())
