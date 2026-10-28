#!/usr/bin/env python3
"""Build, package and catalog the InfinityMods mods. Standard library only; run with `python3 -I`.

  build   <mod> [--zig ZIG] [--out DIR]     compile the mod and its adapter
  verify  <mod> [--zig ZIG]                 build twice in separate folders: same bytes, sane PE, plugin exports
  release [--zig ZIG] --tag TAG --catalog CURRENT.json [--out DIR] [--allow-dirty] [MOD ...]

A recipe's `toolchain` is `zig` (Zig 0.15.2, any host; --zig) or `msvc` (Windows with Visual Studio
C++ x64 tools, for sources that need MSVC, such as `__try`).

Every mod is an ExtraOptions plugin (OD-126/OD-127): its DLL goes to <Azimuth>/mods/<id>/plugins/ and
exports FH2PluginInitialize, its adapter (adapters/<loader>-eo) starting the original loader's entry point.
There is no Infinity mod runtime: no entry needs `infinity-mod-runtime`, and `release` drops that entry.

`release` writes one ZIP per mod (<id>-<version>.zip), each mod's filled manifest and the merged
catalog. A mod whose ZIP has the same SHA-256 as its current catalog entry keeps that entry
unchanged (URL, version and source commit), so players see no update; a changed ZIP needs a new
version. Nothing is uploaded or published.
"""
import argparse
import copy
import os
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
RUNTIME_ID = "infinity-mod-runtime"  # retired (OD-127): never required, dropped from the catalog
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


def version_script(work, recipe):
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
    return script


def version_resource(zig, work, recipe):
    script = version_script(work, recipe)
    resource = work / "version.res"
    subprocess.run([str(zig), "rc", "/fo", resource.name, "--", script.name], check=True, cwd=work)
    return resource


def build(name, zig, out):
    _, recipe, _ = load_mod(name)
    if recipe["toolchain"] == "msvc":
        return build_msvc(recipe, out)
    if zig is None:
        sys.exit(f"{name}: the zig toolchain needs --zig")
    upstream = ROOT / recipe["upstream"]["submodule"]
    adapter = ROOT / "adapters" / recipe["adapter"]
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="infmods-") as temporary:
        work = Path(temporary)
        includes = [adapter, *(upstream / folder for folder in recipe["upstream"]["includes"])]
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


def build_msvc(recipe, out):
    upstream = ROOT / recipe["upstream"]["submodule"]
    adapter = ROOT / "adapters" / recipe["adapter"]
    environment = vcvars()
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="infmods-") as temporary:
        work = Path(temporary)
        script = version_script(work, recipe)
        includes = [adapter, work, *(upstream / folder for folder in recipe["upstream"]["includes"])]
        sources = [upstream / s for s in recipe["upstream"]["sources"]] + sorted(adapter.glob("*.cpp"))
        # Upstream's own MSVC flags (its CMakeLists: /W4 /permissive- /utf-8 /EHsc), static CRT, /Brepro.
        flags = ("/nologo /std:c++20 /utf-8 /EHsc /MT /O2 /W4 /permissive- /Brepro /DUNICODE /D_UNICODE "
                 "/DWIN32_LEAN_AND_MEAN /DNOMINMAX " + " ".join(f'/I"{i}"' for i in includes))
        dll = recipe["dll"]
        (work / "build.cmd").write_text(f"""@echo off
set "PATH=%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer;%PATH%"
call "{environment}" >nul || exit /b 1
rc /nologo /fo version.res {script.name} || exit /b 1
cl {flags} /LD {" ".join(f'"{s}"' for s in sources)} /Fe:{dll} /link /Brepro /OPT:REF /OPT:ICF version.res {" ".join(recipe["upstream"]["libraries"])} || exit /b 1
""", encoding="ascii")
        subprocess.run(["cmd", "/c", str(work / "build.cmd")], check=True, cwd=work)
        shutil.copyfile(work / dll, out / dll)
    return out / dll


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


def layout_problems(recipe, manifest):
    """The launcher's rules for an ExtraOptions mod (Infinity.Core ExtraOptionsMods.CheckPackage, ManagedModFiles):
    every file in <Azimuth>/mods/<id>/, the DLL in plugins/, nothing of ExtraOptions.Infinity's own (winmm.dll,
    FH2_ExtraOptions.ini), and the package's mod.json agreeing with the manifest on id, version and dependencies."""
    problems, folder = [], f"mods/{manifest['id']}/"
    files = manifest["files"]
    if files["game"]:
        problems.append("an ExtraOptions mod installs only into its own mods folder (game files listed)")
    reserved = {"winmm.dll", "fh2_extraoptions.ini"}
    for path in [f["destination"] for f in files["azimuth"]] + [manifest.get("config", {}).get("path", folder)]:
        path = path.replace("\\", "/")
        if not path.startswith(folder) or ".." in path.split("/"):
            problems.append(f"{path} is outside {folder}")
        if path.rsplit("/", 1)[-1].lower() in reserved:
            problems.append(f"{path} would replace ExtraOptions.Infinity's own file")
    binaries = [f["destination"] for f in files["azimuth"] if f["role"] == "binary"]
    if binaries != [f"{folder}plugins/{recipe['dll']}"]:
        problems.append(f"the one binary must be {folder}plugins/{recipe['dll']}, found {binaries}")
    if not any(f["destination"] == f"{folder}mod.json" and f["source"] == "mod.json" for f in files["azimuth"]):
        problems.append(f"{folder}mod.json is not installed")
    try:
        own = json.loads((ROOT / recipe["package"]["mod.json"]).read_text(encoding="utf-8"))
    except (KeyError, OSError, ValueError):
        return problems + ["the package has no readable ExtraOptions mod.json"]
    wanted = {d["id"] for d in manifest.get("dependencies", []) if d["id"] != "extraoptions"}
    if own.get("format_version") != 1 or own.get("id") != manifest["id"] or own.get("version") != manifest["version"]:
        problems.append("the package's mod.json disagrees with the manifest (format_version 1, id, version)")
    if set(own.get("dependencies", [])) != wanted:
        problems.append("the package's mod.json and the manifest declare different dependencies")
    if ("database.json" in recipe["package"]) != (own.get("database") == "database.json"):
        problems.append("mod.json's database and the package's database.json disagree")
    return problems


def verify(name, zig):
    _, recipe, manifest = load_mod(name)
    layout = layout_problems(recipe, manifest)
    with tempfile.TemporaryDirectory(prefix="infmods-a-") as a, tempfile.TemporaryDirectory(prefix="infmods-b-") as b:
        first, second = build(name, zig, Path(a)), build(name, zig, Path(b))
        digests = [hashlib.sha256(p.read_bytes()).hexdigest() for p in (first, second)]
        machine, exports, imports = pe_report(first)
    problems = layout
    if digests[0] != digests[1]:
        problems.append("two builds differ (not reproducible)")
    if machine != 0x8664:
        problems.append("not x64")
    if not recipe["adapter"].endswith("-eo"):
        problems.append(f"adapter {recipe['adapter']} is not an ExtraOptions adapter")
    if "FH2PluginInitialize" not in exports:
        problems.append("FH2PluginInitialize not exported")
    entry = {"fh2-modloader-eo": "FH2ModLoad", "fh2-mod-platform-eo": "fh2mod_init"}.get(recipe["adapter"])
    if entry and entry not in exports:
        problems.append(f"{entry} missing: the adapter has nothing to start")
    retired = [e for e in exports if e.startswith("InfinityMod")]
    if retired:
        problems.append(f"exports the retired Infinity mod runtime's entry points {retired}")
    if ("database.json" in recipe["package"]) != ("FH2PluginDatabaseUpdate" in exports):
        problems.append("FH2PluginDatabaseUpdate export and the package's database.json disagree")
    if "nativeHost" in manifest or manifest.get("runtime") != "standalone":
        problems.append("the manifest must be runtime standalone, without nativeHost")
    if any(d["id"] in ("extraoptions", RUNTIME_ID) for d in manifest.get("dependencies", [])):
        problems.append("the manifest depends on ExtraOptions or the runtime (ExtraOptions is built into Infinity)")
    if "mod.json" not in recipe["package"] or recipe["dll"] not in recipe["package"]:
        problems.append("the package needs the DLL and the ExtraOptions mod.json")
    allowed = {d.lower() for d in recipe["allowedImports"]}
    unexpected = [d for d in imports if d.lower() not in allowed]
    if unexpected:
        problems.append(f"unexpected imports {unexpected}")
    print(f"{recipe['dll']} sha256 {digests[0]}")
    print(f"exports {exports}\nimports {imports}")
    for p in problems:
        print("FAIL", p)
    return 1 if problems else 0


def vcvars():
    if os.name != "nt":
        sys.exit("this step runs on Windows only (WINDOWS)")
    vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
    vs = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires",
                         "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                        check=True, capture_output=True, text=True).stdout.strip()
    if not vs:
        sys.exit("Install Visual Studio C++ x64 tools and the Windows SDK.")
    return Path(vs) / "VC/Auxiliary/Build/vcvars64.bat"


def package(recipe, dll, out):
    """Deterministic ZIP of the recipe's `package` map ("$dll" is the build): fixed time, sorted names.
    Text entries are stored with LF endings, so a Windows checkout (core.autocrlf) packs the same bytes."""
    entries = {name: dll.read_bytes() if source == "$dll" else (ROOT / source).read_bytes().replace(b"\r\n", b"\n")
               for name, source in recipe["package"].items()}
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
        if "package" in file:  # from an additional package (the original's release): pinned in the template
            if file["package"] not in filled.get("additionalPackages", {}) or not file.get("sha256"):
                sys.exit(f"{recipe['id']}: {file['source']} needs a known package and its pinned sha256")
            continue
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
    if manifest.get("additionalPackages"):
        entry["additionalDownloads"] = {name: {k: p[k] for k in ("url", "sha256", "bytes")}
                                        for name, p in manifest["additionalPackages"].items()}
    entry["install"] = {k: v for k, v in manifest.items() if k not in CATALOG_FIELDS}
    return entry


def git(*args):
    # shutil.which finds a git.cmd shim too, which CreateProcess would not resolve by the bare name.
    executable = shutil.which("git") or sys.exit("git not found")
    return subprocess.run([executable, "-C", str(ROOT), *args], check=True, capture_output=True, text=True).stdout.strip()


def release(names, zig, tag, catalog_path, out, allow_dirty):
    if git("status", "--porcelain") and not allow_dirty:
        sys.exit("working tree not clean: the manifests pin the commit the packages are built from")
    commit = git("rev-parse", "HEAD")
    catalog = json.loads(Path(catalog_path).read_text(encoding="utf-8"))
    report = []
    if any(m["id"] == RUNTIME_ID for m in catalog["mods"]):
        catalog["mods"] = [m for m in catalog["mods"] if m["id"] != RUNTIME_ID]
        report.append(f"{RUNTIME_ID}: removed from the catalog (retired)")
    for m in catalog["mods"]:  # entries not built here (external files) keep everything but the runtime
        if RUNTIME_ID in m.get("requires", []):
            m["requires"] = [r for r in m["requires"] if r != RUNTIME_ID]
            if not m["requires"]:
                del m["requires"]
            report.append(f"{m['id']}: no longer requires {RUNTIME_ID}")
    current = {m["id"]: m for m in catalog["mods"]}
    if out.exists():
        shutil.rmtree(out)
    (out / "assets").mkdir(parents=True)
    (out / "manifests").mkdir()
    for name in names or sorted(p.name for p in (ROOT / "mods").iterdir() if (p / "mod.json").exists()):
        _, recipe, manifest = load_mod(name)
        if problems := layout_problems(recipe, manifest):
            sys.exit(f"{name}: " + "; ".join(problems))
        with tempfile.TemporaryDirectory(prefix="infmods-") as temporary:
            dll = build(name, zig, Path(temporary))
            zip_path, contents = package(recipe, dll, out / "assets")
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
            json.dumps(filled, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
        entry = catalog_entry(filled)
        if existing:
            catalog["mods"][catalog["mods"].index(existing)] = entry
        else:
            catalog["mods"].append(entry)
        report.append(f"{recipe['id']}: {'updated' if existing else 'new'} {recipe['version']} at {url}")
    (out / "catalog.json").write_text(json.dumps(catalog, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    (out / "RELEASE.txt").write_text(f"tag {tag}\ncommit {commit}\n" + "\n".join(report) + "\n", encoding="utf-8", newline="\n")
    print("\n".join(report))
    return 0


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="cmd", required=True)
    for cmd in ("build", "verify"):
        p = sub.add_parser(cmd)
        p.add_argument("mod")
        p.add_argument("--zig", type=Path)
        if cmd == "build":
            p.add_argument("--out", type=Path, default=ROOT / "build")
    r = sub.add_parser("release")
    r.add_argument("mods", nargs="*")
    r.add_argument("--zig", type=Path)
    r.add_argument("--tag", required=True)
    r.add_argument("--catalog", required=True, help="the current official catalog.json (read only)")
    r.add_argument("--out", type=Path, default=ROOT / "dist")
    r.add_argument("--allow-dirty", action="store_true", help="development only: the commit pinned is HEAD, not the tree")
    args = parser.parse_args()
    zig = args.zig.resolve() if args.zig else None
    if zig:
        zig_check(zig)
    if args.cmd == "build":
        print(build(args.mod, zig, args.out.resolve()))
        return 0
    if args.cmd == "verify":
        return verify(args.mod, zig)
    return release(args.mods, zig, args.tag, args.catalog, args.out.resolve(), args.allow_dirty)


if __name__ == "__main__":
    sys.exit(main())
