#!/usr/bin/env python3
"""Build, package and catalog the InfinityMods mods. Standard library only; run with `python3 -I`.

  build   <mod> [--zig ZIG] [--out DIR]     compile the mod and its adapter
  verify  <mod> [--zig ZIG]                 build twice in separate folders: same bytes, sane PE, plugin exports
  release [--zig ZIG] --tag TAG [--catalog INDEX] [--out DIR] [--allow-dirty] [MOD ...]
  catalog [--from V2.json]                  re-pin catalog/index.json after editing catalog/ (--from: split a v2 file)
  check   [--catalog FILE]                  validate the catalog (schemas/ + rules below); FILE may be a v2 custom catalog
  external-packs <set folder>... --cars fh2-packs.json --out DIR   ZephyrSkies pack entries (OD-232), read from the ZIPs
  sync    <mod> [--tag TAG] [--zip ZIP --url URL] [--write]   re-pin an FH2 Mod Platform mod to an upstream release
  pin-author <mod> [--zip ZIP] [--write]    re-pin an authorPackage mod to its author's release ZIP

An FH2 Mod Platform mod (adapter fh2-mod-platform-eo) is the author's own release DLL, never built here: the manifest
takes it (adapterConfig.modDll) from the author's release ZIP (additionalPackages.upstream-assets, pinned by SHA-256),
and the package holds only the adapter, which loads it. `sync` updates those pins from the release the recipe's
upstreamRelease names (.claude/skills/upstream-sync/SKILL.md). The exception is a recipe with `modBuild` (Underglow,
owner exception to the no-game-material rule for its camera_presets.h): the mod's DLL is built here from the pinned
submodule commit with the recipe's patches applied (`git apply`, never in the submodule itself), and shipped in the
package at adapterConfig.modDll.

A recipe with `authorPackage` (Garage Revving and Weather Control, owner 2026-11-16) is the author's own release
files, neither built nor packaged here: its catalog download IS the author's release ZIP (authorPackage.repository,
.tag, .asset), pinned by SHA-256, and every installed file, the author's mod.json included, is an entry of that ZIP,
pinned too. `pin-author` re-pins it after the author replaces the asset; `release` takes its manifest as it is.

A recipe with `source` instead of `upstream` (EasyRadio) is Infinity's own mod: its source is in this repository
(source.path, e.g. mods/easyradio), not a submodule, and builds with the same fields (sources, includes, extraFlags,
importDefinitions, linkFlags).

A recipe's `toolchain` is `zig` (Zig 0.15.2, any host; --zig) or `msvc` (Windows with Visual Studio
C++ x64 tools, for sources that need MSVC, such as `__try`).

Every mod is an ExtraOptions plugin (OD-126/OD-127): its DLL goes to <Azimuth>/mods/<game folder>/<id>/plugins/
(ExtraOptions 0.6.0's per-game mods folders, OD-233: mods/Forza Horizon 2/ or mods/Forza Motorsport 5/, chosen by
the manifest's `games`, FH2 when it declares none) and exports FH2PluginInitialize: its adapter (adapters/<loader>-eo)
starts the original loader's entry point, or, for an ExtraOptions plugin itself (`adapter` null), the upstream source
exports it. There is no Infinity mod runtime: no entry needs `infinity-mod-runtime`, and `release` drops that entry.

The catalog is split (schema 3, schemas/catalog-v3.schema.json): catalog/index.json lists each mod's card details
(id, version, name, description, author) and pins its detail file catalog/<id>.json (path, sha256, bytes); the
detail file holds the rest of its schema-2 entry. catalog/ is what people edit; `catalog` re-pins the index after an
edit. The schema-2 catalog.json was retired once every launcher read schema 3 (floor 3.1.7).

`release` writes one ZIP per mod (<id>-<version>.zip), each mod's filled manifest and the merged
catalog (dist/catalog/). A mod whose ZIP has the same SHA-256 as its current catalog entry keeps that entry
unchanged (URL, version and source commit), so players see no update; a changed ZIP needs a new
version. Nothing is uploaded or published.

`check` validates what launchers read from this repository's main branch: catalog/index.json and every detail
file against schemas/catalog-v3.schema.json (the subset of JSON Schema it uses), with no BOM, no duplicate JSON keys and
the launcher's size limits, each pin matching its file, ids unique, no detail file left unlisted, every installed
file in its game's mods folder. `check --catalog FILE` validates any one catalog file too, a schema-2 custom catalog
included (schemas/catalog.schema.json). `check` also applies the launcher's settings rules (option types, folder options,
config.openFolders) and the launcher floor: an entry that uses a launcher feature listed in release.json
`launcherFeatures` is refused until that feature's `since` names the first launcher release with it (filled in once
the platform's launcher floor reaches it), because an older launcher refuses the whole catalog, not one entry.
OD-233 retired the parity exemptions
(parity-non-required.json): a mod is either both modes (its multiplayer impact cosmetic) or Noob only.
"""
import argparse
import copy
import os
import hashlib
import io
import json
import re
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
# OD-233: ExtraOptions 0.6.0's per-game mods folders (shared/addon_layout.h), by the server's game id
GAME_FOLDERS = {"fh2": "Forza Horizon 2", "fm5": "Forza Motorsport 5"}


def mod_folder(manifest):
    """The mod's own folder, Azimuth-relative: mods/<game folder>/<id>/ (its one game; FH2 when it declares none)."""
    games = manifest.get("games", ["fh2"])
    if len(games) != 1 or games[0] not in GAME_FOLDERS:
        sys.exit(f"{manifest['id']}: a mod here is for exactly one of {sorted(GAME_FOLDERS)}, found {games}")
    return f"mods/{GAME_FOLDERS[games[0]]}/{manifest['id']}/"


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
    if recipe.get("authorPackage"):
        sys.exit(f"{name}: the author's own release files (authorPackage), nothing to build; see pin-author")
    if recipe["toolchain"] == "msvc":
        return build_msvc(recipe, out)
    if zig is None:
        sys.exit(f"{name}: the zig toolchain needs --zig")
    # Infinity's own mods (EasyRadio) keep their source here (recipe `source`, its `path` a folder of this repository);
    # every other mod builds an upstream submodule (recipe `upstream`). Both blocks take the same build fields.
    code = recipe.get("source") or recipe["upstream"]
    upstream = ROOT / (code["path"] if "source" in recipe else code["submodule"])
    # An ExtraOptions plugin itself (`adapter` null) exports FH2PluginInitialize from its own source.
    adapter = ROOT / "adapters" / recipe["adapter"] if recipe.get("adapter") else None
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="infmods-") as temporary:
        work = Path(temporary)
        includes = [*([adapter] if adapter else []), *(upstream / folder for folder in code["includes"])]
        # Upstream's own flags (build.py), so its sources compile exactly as they do there; a recipe adds what its build.py adds.
        flags = [str(zig), "clang", "-target", "x86_64-w64-windows-gnu", "-c", "-ffreestanding", "-fno-builtin",
                 "-fno-stack-protector", "-fms-extensions", "-funwind-tables", "-O2", "-Wall", "-Wextra", "-Werror",
                 *code.get("extraFlags", []),
                 *(a for folder in includes for a in ("-I", str(folder))),
                 "-isystem", str(zig.parent / "lib/include"),
                 "-isystem", str(zig.parent / "lib/libc/include/any-windows-any")]
        if code.get("forceInclude"):
            flags += ["-include", str(upstream / code["forceInclude"])]
        sources = [upstream / s for s in code["sources"]] + (sorted(adapter.glob("*.c")) if adapter else [])
        objects = []
        for index, source in enumerate(sources):
            obj = f"{index:02d}-{source.stem}.obj"  # relative: the PDB records these names, not the temp folder
            subprocess.run([*flags, str(source), "-o", obj], check=True, cwd=work)
            objects.append(obj)
        libraries = []
        for definition in code.get("importDefinitions", []):
            library = Path(definition).stem + ".lib"
            subprocess.run([str(zig), "dlltool", "-m", "i386:x86-64", "-d", str(upstream / definition), "-l", library],
                           check=True, cwd=work)
            libraries.append(library)
        for name in code.get("systemImports", []):
            # Upstream's own way (weather-control/build.py): an import library of the Windows DLL's export table, so no
            # second C runtime is linked into a game plugin. Windows hosts only (WINDOWS builds the releases).
            if os.name != "nt":
                sys.exit(f"{recipe['id']}: systemImports read %WINDIR%\\System32 (Windows only)")
            _, exports, _ = pe_report(Path(os.environ["WINDIR"]) / "System32" / (name + ".dll"))
            (work / (name + ".def")).write_text(f"LIBRARY {name}.dll\nEXPORTS\n" + "\n".join(exports) + "\n", encoding="ascii")
            subprocess.run([str(zig), "dlltool", "-m", "i386:x86-64", "-d", name + ".def", "-l", name + ".lib"], check=True, cwd=work)
            libraries.append(name + ".lib")
        dll = out / recipe["dll"]
        subprocess.run([str(zig), "lld-link", "/dll", "/entry:DllMain", "/machine:x64", "/nodefaultlib", "/dynamicbase",
                        "/nxcompat", "/Brepro", "/out:" + dll.name, "/implib:mod.lib", *objects, *libraries,
                        version_resource(zig, work, recipe).name, "/release", "/debug", "/pdb:" + dll.stem + ".pdb",
                        "/pdbaltpath:%_PDB%", "/pdbsourcepath:C:\\infinity-mods",
                        # what upstream's build.py adds at link time (e.g. /opt:ref with -ffunction-sections)
                        *code.get("linkFlags", [])], check=True, cwd=work)
        shutil.copyfile(work / dll.name, dll)
    return dll


def adapter_config(work, recipe):
    """fh2_mod_platform_eo_config.h: the mod's own release DLL that the FH2 Mod Platform adapter loads, relative to the
    mod folder (recipe `adapterConfig.modDll`; outside plugins/, so ExtraOptions never loads it itself)."""
    if recipe.get("adapter") != "fh2-mod-platform-eo":
        return
    mod_dll = recipe.get("adapterConfig", {}).get("modDll", "")
    if (not re.fullmatch(r"[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)*\.dll", mod_dll) or mod_dll.lower().startswith("plugins/")
            or any(part in (".", "..") for part in mod_dll.split("/"))):
        sys.exit(f"{recipe['id']}: adapterConfig.modDll must be a relative .dll path outside plugins/, found {mod_dll!r}")
    literal = mod_dll.replace("/", "\\\\")  # a C string literal: fh2-mods\\FnFNitro.dll
    (work / "fh2_mod_platform_eo_config.h").write_text(
        "#pragma once\n"
        f'#define FH2MP_EO_MOD_DLL L"{literal}"\n'
        f'#define FH2MP_EO_MOD_DLL_TEXT "{literal}"\n', encoding="ascii", newline="\n")


def build_msvc(recipe, out):
    upstream = ROOT / recipe["upstream"]["submodule"]
    adapter = ROOT / "adapters" / recipe["adapter"]
    environment = vcvars()
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="infmods-") as temporary:
        work = Path(temporary)
        script = version_script(work, recipe)
        adapter_config(work, recipe)
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


def build_mod(recipe, out):
    """The mod's own DLL (recipe `modBuild`, MSVC): the pinned submodule commit's files (git archive, LF, so the
    patches apply the same on any checkout), the recipe's patches applied in order, compiled with upstream's MSVC
    flags (its CMakeLists: /W4 /permissive- /utf-8 /EHsc, static CRT) plus /Brepro; relative paths only, so two
    builds in different folders give the same bytes."""
    import tarfile
    spec = recipe["modBuild"]
    upstream = ROOT / recipe["upstream"]["submodule"]
    environment = vcvars()
    out.mkdir(parents=True, exist_ok=True)
    name = Path(spec["output"]).name
    with tempfile.TemporaryDirectory(prefix="infmods-mod-") as temporary:
        work = Path(temporary)
        tree = work / "src"
        tree.mkdir()
        archive = subprocess.run([shutil.which("git") or sys.exit("git not found"), "-c", "core.autocrlf=false",
                                  "-c", "core.eol=lf", "-C", str(upstream), "archive", "--format=tar", "HEAD"],
                                 check=True, capture_output=True).stdout
        with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
            tar.extractall(tree, filter="data")
        for patch in spec.get("patches", []):
            subprocess.run([shutil.which("git"), "apply", "--whitespace=nowarn", "-p1", str(ROOT / patch)], check=True, cwd=tree)
        # Headers outside the archive (the platform submodule), by path relative to the work folder: copied in.
        for folder in spec.get("includes", []):
            shutil.copytree(upstream / folder, work / "inc" / folder)
        includes = " ".join(f'/I"inc/{folder}"' for folder in spec.get("includes", []))
        defines = " ".join(f'/D{d}' for d in spec.get("defines", []))
        sources = " ".join(f'"src/{s}"' for s in spec["sources"])
        flags = ("/nologo /std:c++20 /utf-8 /EHsc /MT /O2 /DNDEBUG /W4 /permissive- /Brepro /DUNICODE /D_UNICODE "
                 "/DWIN32_LEAN_AND_MEAN /DNOMINMAX")
        (work / "build.cmd").write_text(f"""@echo off
set "PATH=%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer;%PATH%"
call "{environment}" >nul || exit /b 1
cl {flags} {defines} {includes} /LD {sources} /Fe:{name} /link /Brepro /OPT:REF /OPT:ICF {" ".join(spec.get("libraries", []))} || exit /b 1
""", encoding="ascii")
        subprocess.run(["cmd", "/c", str(work / "build.cmd")], check=True, cwd=work)
        shutil.copyfile(work / name, out / name)
    return out / name


def verify_mod_build(recipe, a, b):
    """Two builds of the mod's own DLL give the same bytes; x64, fh2mod_init, imports in modBuild.allowedImports."""
    first, second = build_mod(recipe, a / "mod"), build_mod(recipe, b / "mod")
    digests = [hashlib.sha256(p.read_bytes()).hexdigest() for p in (first, second)]
    machine, exports, imports = pe_report(first)
    problems = []
    if digests[0] != digests[1]:
        problems.append("two builds of the mod's DLL differ (not reproducible)")
    if machine != 0x8664:
        problems.append("the mod's DLL is not x64")
    if "fh2mod_init" not in exports:
        problems.append("the mod's DLL does not export fh2mod_init")
    allowed = {d.lower() for d in recipe["modBuild"]["allowedImports"]}
    if extra := [d for d in imports if d.lower() not in allowed]:
        problems.append(f"the mod's DLL imports {extra}, not in modBuild.allowedImports")
    print(f"{first.name} sha256 {digests[0]}\nmod exports {exports}\nmod imports {imports}")
    return problems


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
    every file in <Azimuth>/mods/<game folder>/<id>/ (OD-233), the DLL in plugins/, nothing of ExtraOptions.Infinity's
    own (winmm.dll, FH2_ExtraOptions.ini), and the package's mod.json agreeing with the manifest on id, version and
    dependencies."""
    problems, folder = [], mod_folder(manifest)
    files = manifest["files"]
    if files["game"]:
        problems.append("an ExtraOptions mod installs only into its own mods folder (game files listed)")
    reserved = {"winmm.dll", "fh2_extraoptions.ini"}
    # Infinity.Core ModManifest: file roles and modes, and a config-role file only as the settings form's own file
    config_path = (manifest.get("config") or {}).get("path")
    for f in files["azimuth"]:
        if f["role"] not in ("binary", "config", "asset", "user-asset", "license") or f["mode"] not in ("add", "replace", "add-or-replace", "seed") \
                or f["mode"] == "seed" and f["role"] not in ("config", "user-asset") \
                or f["role"] == "config" and (f["mode"] != "seed" or f["destination"] != config_path) \
                or config_path and f["destination"].lower() == config_path.lower() and f["role"] != "config":
            problems.append(f"{f['destination']}: invalid role {f['role']}/mode {f['mode']} (a config file is the config block's path, seeded)")
    for path in [f["destination"] for f in files["azimuth"]] + [(manifest.get("config") or {}).get("path", folder)]:
        path = path.replace("\\", "/")
        if not path.startswith(folder) or ".." in path.split("/"):
            problems.append(f"{path} is outside {folder}")
        if path.rsplit("/", 1)[-1].lower() in reserved:
            problems.append(f"{path} would replace ExtraOptions.Infinity's own file")
    if recipe.get("authorPackage"):  # every file is the author's ZIP's; pin-author checks their mod.json against it
        binaries = [f["destination"] for f in files["azimuth"] if f["role"] == "binary"]
        if binaries != [f"{folder}plugins/{recipe['dll']}"]:
            problems.append(f"the one binary must be {folder}plugins/{recipe['dll']}, found {binaries}")
        if any("package" in f for f in files["azimuth"]) or manifest.get("additionalPackages"):
            problems.append("an authorPackage mod installs from the author's ZIP alone (no additional packages)")
        if not any(f["destination"] == f"{folder}mod.json" for f in files["azimuth"]):
            problems.append(f"{folder}mod.json is not installed")
        return problems
    binaries = [f["destination"] for f in files["azimuth"] if f["role"] == "binary" and "package" not in f]
    mod_dll = recipe.get("adapterConfig", {}).get("modDll")
    built = recipe.get("modBuild")
    if built and built.get("output") != mod_dll:
        problems.append("modBuild.output must be adapterConfig.modDll")
    wanted_built = sorted([f"{folder}plugins/{recipe['dll']}"] + ([f"{folder}{mod_dll}"] if built else []))
    if sorted(binaries) != wanted_built:
        problems.append(f"the built binaries must be {wanted_built}, found {binaries}")
    # The FH2 Mod Platform adapter starts the original author's release DLL, downloaded from their release (an
    # additional package, pinned by SHA-256): never built or shipped here.
    from_release = [f["destination"] for f in files["azimuth"] if f["role"] == "binary" and "package" in f]
    if from_release != ([f"{folder}{mod_dll}"] if mod_dll and not built else []):
        problems.append(f"the binary from the original release must be exactly adapterConfig.modDll, found {from_release}")
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
    if recipe.get("authorPackage"):
        sys.exit(f"{name}: the author's own release files (authorPackage), nothing to build; see pin-author")
    layout = layout_problems(recipe, manifest)
    with tempfile.TemporaryDirectory(prefix="infmods-a-") as a, tempfile.TemporaryDirectory(prefix="infmods-b-") as b:
        first, second = build(name, zig, Path(a)), build(name, zig, Path(b))
        digests = [hashlib.sha256(p.read_bytes()).hexdigest() for p in (first, second)]
        machine, exports, imports = pe_report(first)
        mod_problems = verify_mod_build(recipe, Path(a), Path(b)) if recipe.get("modBuild") else []
    problems = layout + mod_problems
    if digests[0] != digests[1]:
        problems.append("two builds differ (not reproducible)")
    if machine != 0x8664:
        problems.append("not x64")
    if recipe.get("adapter") is not None and not recipe["adapter"].endswith("-eo"):
        problems.append(f"adapter {recipe['adapter']} is not an ExtraOptions adapter")
    if "FH2PluginInitialize" not in exports:
        problems.append("FH2PluginInitialize not exported")
    entry = {"fh2-modloader-eo": "FH2ModLoad"}.get(recipe["adapter"])
    if entry and entry not in exports:
        problems.append(f"{entry} missing: the adapter has nothing to start")
    if recipe["adapter"] == "fh2-mod-platform-eo":
        # The mod's fh2mod_init is in its own release DLL (adapterConfig.modDll); the adapter serves set_file.
        if "fh2mod_init" in exports:
            problems.append("the adapter exports fh2mod_init: the mod is linked in instead of loaded from its release")
        if "FH2PluginMediaDirectory" not in exports:
            problems.append("FH2PluginMediaDirectory missing: set_file has nowhere to go")
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


def package(recipe, dll, out, mod_dll=None):
    """Deterministic ZIP of the recipe's `package` map ("$dll" is the build): fixed time, sorted names.
    Text entries are stored with LF endings, so a Windows checkout (core.autocrlf) packs the same bytes."""
    generated = GENERATORS[recipe["generate"]](recipe) if recipe.get("generate") else {}
    entries = {name: dll.read_bytes() if source == "$dll" else mod_dll.read_bytes() if source == "$modDll"
               else generated[name] if source == "$generated"
               else (ROOT / source).read_bytes().replace(b"\r\n", b"\n")
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


def weather_control_pages(recipe):
    """Upstream's weather-control/build.py page step, unchanged: one native page per EO language from its template and
    translations.json (the XAML its plugin loads from plugins/EO_WeatherControl/Media), UTF-8 with a BOM and CRLF as it writes them on Windows."""
    from xml.sax.saxutils import escape
    source = ROOT / recipe["upstream"]["submodule"] / recipe["upstream"]["includes"][0]
    catalog = json.loads((source / "translations.json").read_text(encoding="utf8"))
    keys = ["TITLE", "LOCK", "HOUR", "WEATHER", "LOCK_DESC", "HOUR_DESC", "WEATHER_DESC"]
    template = (source / "weather_page.template.xaml").read_text(encoding="utf8").replace("\r\n", "\n")
    pages = {}
    for code, values in catalog.items():
        text = template
        for key, value in zip(keys, values):
            text = text.replace("@" + key + "@", escape(value, {'"': "&quot;"}))
        pages[f"EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_{code}.xaml"] = b"\xef\xbb\xbf" + text.replace("\n", "\r\n").encode("utf-8")
    return pages


GENERATORS = {"weather-control-pages": weather_control_pages}


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


CATALOG_DIR = ROOT / "catalog"
INDEX_NAME = "index.json"
CARD_FIELDS = ("id", "version", "name", "description", "author")
DETAIL_MAX_BYTES = 256 * 1024  # Infinity.Core: a schema-3 detail file's limit


def dumps(value):
    return (json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def detail_of(entry):
    """A schema-2 entry's detail file (schema 3): everything but the card text, with the entry's id and version."""
    return {"schemaVersion": 3, "id": entry["id"], "version": entry["version"],
            **{k: v for k, v in entry.items() if k not in CARD_FIELDS}}


def entry_of(card, detail):
    """The schema-2 entry an index card and its detail file stand for (the launcher rebuilds the same one)."""
    return {**{k: card[k] for k in CARD_FIELDS}, **{k: v for k, v in detail.items() if k not in ("schemaVersion", "id", "version")}}


def split_catalog(catalog):
    """A schema-2 catalog as (index bytes, {file name: detail bytes})."""
    details, cards = {}, []
    for entry in catalog["mods"]:
        raw = dumps(detail_of(entry))
        name = f"{entry['id']}.json"
        details[name] = raw
        cards.append({**{k: entry[k] for k in CARD_FIELDS}, "path": name,
                      "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)})
    return dumps({"schemaVersion": 3, "mods": cards}), details


def write_catalog(catalog, out):
    """Writes out/catalog/: the index and one detail file per mod (any other .json there removed)."""
    index, details = split_catalog(catalog)
    folder = out / "catalog"
    folder.mkdir(parents=True, exist_ok=True)
    for stale in folder.glob("*.json"):
        if stale.name != INDEX_NAME and stale.name not in details:
            stale.unlink()
    for name, raw in details.items():
        (folder / name).write_bytes(raw)
    (folder / INDEX_NAME).write_bytes(index)
    return len(index)


def load_catalog(path):
    """A catalog as its schema-2 form: a v2 file as it is, a v3 index merged with its detail files (local paths only)."""
    path = Path(path)
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("schemaVersion") != 3:
        return document
    mods = []
    for card in document["mods"]:
        if card["path"].startswith("https://"):
            sys.exit(f"{card['id']}: a remote detail file; the tool reads local catalogs only")
        detail = json.loads((path.parent / card["path"]).read_text(encoding="utf-8"))
        if (detail.get("id"), detail.get("version")) != (card["id"], card["version"]):
            sys.exit(f"{card['path']}: id/version {detail.get('id')} {detail.get('version')} differ from the index's"
                     f" {card['id']} {card['version']}; change both")
        mods.append(entry_of(card, detail))
    return {"schemaVersion": 2, "mods": mods}


def catalog_command(from_v2):
    """Re-pins catalog/index.json after an edit of catalog/; --from splits a schema-2 file into catalog/."""
    catalog = json.loads(Path(from_v2).read_text(encoding="utf-8")) if from_v2 else load_catalog(CATALOG_DIR / INDEX_NAME)
    if from_v2 is None:  # the index's card text wins; details come from the files as edited
        index = json.loads((CATALOG_DIR / INDEX_NAME).read_text(encoding="utf-8"))
        listed = {c["path"] for c in index["mods"]}
        unlisted = sorted(p.name for p in CATALOG_DIR.glob("*.json") if p.name != INDEX_NAME and p.name not in listed)
        if unlisted:
            sys.exit("not in catalog/index.json (add a card for each, or delete them): " + ", ".join(unlisted))
    index_bytes = write_catalog(catalog, ROOT)
    print(f"catalog/{INDEX_NAME} {index_bytes} bytes of {CATALOG_MAX_BYTES}, {len(catalog['mods'])} detail files")
    return check(CATALOG_DIR / INDEX_NAME)


def git(*args):
    # shutil.which finds a git.cmd shim too, which CreateProcess would not resolve by the bare name.
    executable = shutil.which("git") or sys.exit("git not found")
    return subprocess.run([executable, "-C", str(ROOT), *args], check=True, capture_output=True, text=True).stdout.strip()


def release(names, zig, tag, catalog_path, out, allow_dirty):
    if git("status", "--porcelain") and not allow_dirty:
        sys.exit("working tree not clean: the manifests pin the commit the packages are built from")
    commit = git("rev-parse", "HEAD")
    catalog = load_catalog(catalog_path)
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
        existing = current.get(recipe["id"])
        if recipe.get("authorPackage"):  # the author's ZIP, pinned by pin-author: nothing built, nothing to attach
            if not manifest["package"]["sha256"] or any(not f.get("sha256") for f in manifest["files"]["azimuth"]):
                sys.exit(f"{name}: not pinned; run pin-author {name} --write")
            entry = catalog_entry(manifest)
            if existing == entry:
                report.append(f"{recipe['id']}: unchanged, keeps {existing['download']['url']}")
                continue
            if existing and existing.get("version") == recipe["version"] and existing.get("download") != entry["download"]:
                sys.exit(f"{recipe['id']}: the author's package changed but version {recipe['version']} did not; bump it")
            if existing:
                catalog["mods"][catalog["mods"].index(existing)] = entry
            else:
                catalog["mods"].append(entry)
            report.append(f"{recipe['id']}: {'updated' if existing else 'new'} {recipe['version']}, the author's {manifest['package']['url']}")
            continue
        with tempfile.TemporaryDirectory(prefix="infmods-") as temporary:
            dll = build(name, zig, Path(temporary))
            mod_dll = build_mod(recipe, Path(temporary) / "mod") if recipe.get("modBuild") else None
            zip_path, contents = package(recipe, dll, out / "assets", mod_dll)
        digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
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
    write_catalog(catalog, out)
    (out / "RELEASE.txt").write_text(f"tag {tag}\ncommit {commit}\n" + "\n".join(report) + "\n", encoding="utf-8", newline="\n")
    print("\n".join(report))
    return 0


CATALOG_MAX_BYTES = 1024 * 1024  # Infinity.Core OfficialModCatalog.MaxCatalogBytes


def schema_errors(value, schema, root, where="$"):
    """The JSON Schema keywords schemas/ uses (type, const, enum, pattern, length and item bounds, required,
    properties, additionalProperties, items, uniqueItems, minimum/maximum, not, anyOf, oneOf, local $ref)."""
    if "$ref" in schema:
        target = root
        for part in schema["$ref"].removeprefix("#/").split("/"):
            target = target[part]
        return schema_errors(value, target, root, where)
    errors = []
    kinds = {"object": dict, "array": list, "string": str, "boolean": bool}
    kind = schema.get("type")
    if kind == "integer":
        if isinstance(value, bool) or not isinstance(value, int):
            return [f"{where}: not an integer"]
    elif kind and not isinstance(value, kinds[kind]):
        return [f"{where}: not {'an' if kind[0] in 'ao' else 'a'} {kind}"]
    if "const" in schema and (value != schema["const"] or type(value) is not type(schema["const"])):
        errors.append(f"{where}: must be {json.dumps(schema['const'])}")
    if "enum" in schema and value not in schema["enum"]:
        errors.append(f"{where}: not one of {schema['enum']}")
    if isinstance(value, str):
        # ECMA-262 "$" never matches before a final newline; Python's does, so the anchor becomes \Z
        if "pattern" in schema and not re.search(schema["pattern"].replace("$", r"\Z"), value):
            errors.append(f"{where}: {value!r} does not match {schema['pattern']}")
        if len(value) < schema.get("minLength", 0) or len(value) > schema.get("maxLength", len(value)):
            errors.append(f"{where}: length out of bounds")
    if isinstance(value, int) and not isinstance(value, bool):
        if value < schema.get("minimum", value) or value > schema.get("maximum", value):
            errors.append(f"{where}: {value} out of bounds")
    if isinstance(value, list):
        if len(value) < schema.get("minItems", 0) or len(value) > schema.get("maxItems", len(value)):
            errors.append(f"{where}: {len(value)} items, out of bounds")
        if schema.get("uniqueItems") and len({json.dumps(v, sort_keys=True) for v in value}) != len(value):
            errors.append(f"{where}: repeated items")
        if "items" in schema:
            for index, item in enumerate(value):
                errors += schema_errors(item, schema["items"], root, f"{where}[{index}]")
    if isinstance(value, dict):
        errors += [f"{where}: missing {key}" for key in schema.get("required", []) if key not in value]
        properties = schema.get("properties", {})
        extra = schema.get("additionalProperties", True)
        for key, item in value.items():
            if key in properties:
                errors += schema_errors(item, properties[key], root, f"{where}.{key}")
            elif extra is False:
                errors.append(f"{where}: unknown field {key}")
            elif isinstance(extra, dict):
                errors += schema_errors(item, extra, root, f"{where}.{key}")
    if "not" in schema and not schema_errors(value, schema["not"], root, where):
        errors.append(f"{where}: matches a form it must not have")
    if "anyOf" in schema and all(schema_errors(value, s, root, where) for s in schema["anyOf"]):
        errors.append(f"{where}: matches none of the allowed forms")
    if "oneOf" in schema and sum(not schema_errors(value, s, root, where) for s in schema["oneOf"]) != 1:
        errors.append(f"{where}: must match exactly one of the allowed forms")
    return errors


def read_strict(path, limit):
    """The launcher's reading rules: UTF-8 without BOM, at most `limit` bytes, no repeated key in any object."""
    raw = Path(path).read_bytes()
    if raw.startswith(b"\xef\xbb\xbf"):
        raise ValueError("starts with a BOM")
    if not 0 < len(raw) <= limit:
        raise ValueError(f"{len(raw)} bytes (limit {limit})")

    def pairs(items):
        keys = [k for k, _ in items]
        if len(keys) != len(set(keys)):
            raise ValueError(f"repeated key in an object: {sorted({k for k in keys if keys.count(k) > 1})}")
        return dict(items)

    return json.loads(raw.decode("utf-8"), object_pairs_hook=pairs)


def layout_problem(entry):
    """OD-233: every file in its game's mods folder (the launcher still reads the old mods/<id>/ form, as its game's)."""
    install = entry["install"]
    folder = mod_folder({"id": entry["id"], **({"games": install["games"]} if "games" in install else {})})
    config_path = (install.get("config") or {}).get("path")
    for f in install.get("files", {}).get("azimuth", []):
        if not f["destination"].startswith(folder):
            return f"{f['destination']} is outside {folder}"
        # Infinity.Core ModManifest: a seeded file is config or user-asset; a config file is the config block's path
        if f["mode"] == "seed" and f["role"] not in ("config", "user-asset") \
                or f["role"] == "config" and (f["mode"] != "seed" or f["destination"] != config_path) \
                or config_path and f["destination"].lower() == config_path.lower() and f["role"] != "config":
            return f"{f['destination']}: invalid role {f['role']}/mode {f['mode']} for its settings mapping"
    return None


# Infinity.Core ModManifest / ModFolderOption (InfinityHorizon src/Infinity.Core/Mods, batch 1118): the option types a
# launcher reads, and the settings rules check mirrors. Launchers older than a type refuse every manifest that uses it,
# and the official catalog loads all-or-nothing (OfficialModCatalog.MergedManifest: "invalid catalog: <id>: ...", the
# cached catalog stays shown), so an entry with a newer type must wait for the platform's launcher floor.
OPTION_TYPES = ("boolean", "integer", "number", "enum", "folder")
TOKEN = re.compile(r"[A-Za-z0-9_-]{1,64}")
DEVICE_NAMES = {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(1, 10)), *(f"LPT{i}" for i in range(1, 10))}


def launcher_features(install):
    """The release.json launcherFeatures an install body uses (settings folders: folder options, config.openFolders)."""
    config = install.get("config") or {}
    uses = set()
    if any(o.get("type") == "folder" for o in config.get("options", [])) or "openFolders" in config:
        uses.add("settingsFolders")
    return uses


def floor_problems(entry):
    """An entry that uses a launcher feature needs release.json launcherFeatures.<feature>.since filled in: the first
    launcher release with it, once the platform's launcher floor is at least that release. Until then: refused."""
    problems = []
    for feature in sorted(launcher_features(entry["install"])):
        since = RELEASE.get("launcherFeatures", {}).get(feature, {}).get("since")
        if not (isinstance(since, str) and re.fullmatch(r"\d+\.\d+\.\d+", since)):
            problems.append(f"uses launcher feature {feature}, whose launcher floor is not filled in (release.json "
                            f"launcherFeatures.{feature}.since is {since!r}): older launchers refuse the whole catalog; set it to the "
                            f"first launcher release with the feature once the platform's launcher floor is at least that version")
    return problems


def azimuth_folder_ok(path):
    """ModManifest.AzimuthFolder: ValidatePath (relative, '/', no 8.3 alias, no device name) and not under mods/."""
    if not isinstance(path, str) or not path.strip() or "\\" in path or ":" in path or path.startswith("/") \
            or any(ord(c) < 32 or c in '<>"|?*' for c in path):
        return False
    for part in path.split("/"):
        tilde = part.find("~")
        if part in ("", ".", "..") or part.endswith((".", " ")) or part.split(".")[0].upper() in DEVICE_NAMES \
                or 0 <= tilde < len(part) - 1 and part[tilde + 1].isdigit():
            return False
    return path.split("/")[0].lower() != "mods"


def localized_ok(text):
    """ModManifest.ValidateLocalized: an object with en, every value a non-empty string of at most 4096 characters."""
    return isinstance(text, dict) and "en" in text and all(isinstance(v, str) and 0 < len(v) <= 4096 for v in text.values())


def settings_problems(entry):
    """The settings rules of ModManifest (4e4ac2a+): option types, enum choice labels, folder options, openFolders, and the
    places a folder option may never be named (multiplayer.settings, a catalog's required.settings, a dependency's settings)."""
    install, problems = entry["install"], []
    config = install.get("config") or {}
    options = config.get("options", [])
    folders = set()
    for o in options:
        where, kind = f"option {o.get('id')}", o.get("type")
        if kind not in OPTION_TYPES:
            problems.append(f"{where}: type {kind!r} is not one of {OPTION_TYPES}")
            continue
        if kind == "enum":
            if any("label" in c and not localized_ok(c["label"]) for c in o.get("choices", [])):
                problems.append(f"{where}: a choice label must be localized text with en")
            continue
        if kind != "folder":
            continue
        folders.add(o.get("id"))
        empty = o.get("emptyValue")
        if not (isinstance(empty, str) and TOKEN.fullmatch(empty)) or not localized_ok(o.get("emptyLabel")):
            problems.append(f"{where}: a folder needs an emptyValue token and a localized emptyLabel with en")
            continue
        tokens = o.get("tokens")
        if tokens is not None:
            lowered = [t.lower() for t in tokens if isinstance(t, str)]
            if not isinstance(tokens, list) or not 1 <= len(tokens) <= 64 or len(lowered) != len(tokens) \
                    or not all(TOKEN.fullmatch(t) for t in tokens) or empty.lower() in lowered or len(set(lowered)) != len(lowered):
                problems.append(f"{where}: tokens must be 1-64 distinct tokens (any case), none the emptyValue")
            if not azimuth_folder_ok(o.get("tokenFolder")):
                problems.append(f"{where}: tokens need a tokenFolder, Azimuth-relative and not under mods/")
        elif "tokenFolder" in o:
            problems.append(f"{where}: tokenFolder without tokens")
        default = o.get("default")
        if not isinstance(default, str) or default.lower() not in {empty.lower(), *(t.lower() for t in tokens or [] if isinstance(t, str))}:
            problems.append(f"{where}: a folder's default is its emptyValue or one of its tokens")
        if "lockedValue" in o or "choices" in o:
            problems.append(f"{where}: a folder takes no lockedValue and no choices")
    if "openFolders" in config:
        items = config["openFolders"]
        if not isinstance(items, list) or len(items) > 8:
            problems.append("openFolders: at most 8")
            items = []
        seen = set()
        for item in items:
            path = item.get("path") if isinstance(item, dict) else None
            if not isinstance(item, dict) or set(item) - {"root", "path", "label", "create"} or item.get("root") != "azimuth" \
                    or not azimuth_folder_ok(path) or not localized_ok(item.get("label")) \
                    or not isinstance(item.get("create", False), bool) or path.lower() in seen:
                problems.append(f"openFolders: invalid entry {item!r}")
                continue
            seen.add(path.lower())
    multiplayer = install.get("multiplayer") or {}
    problems += [f"multiplayer.settings names the folder option {i}" for i in multiplayer.get("settings", []) if i in folders]
    problems += [f"required.settings names the folder option {r.get('optionId')}" for r in (entry.get("required") or {}).get("settings", [])
                 if isinstance(r, dict) and r.get("optionId") in folders]
    return problems


def check_v2(path, name, problems):
    """A schema-2 catalog file (a custom catalog): its schema, unique ids, layout. Returns it, or None."""
    schema = json.loads((ROOT / "schemas" / "catalog.schema.json").read_text(encoding="utf-8"))
    try:
        document = read_strict(path, CATALOG_MAX_BYTES)
    except (OSError, ValueError) as error:
        problems.append(f"{name}: {error}")
        return None
    if errors := schema_errors(document, schema, schema):
        problems += [f"{name}: {e}" for e in errors]
        return None
    ids = [m["id"] for m in document["mods"]]
    problems += [f"{name}: id {i} listed more than once" for i in sorted(set(ids)) if ids.count(i) > 1]
    problems += [f"{name}: {m['id']}: {p}" for m in document["mods"] if (p := layout_problem(m))]
    problems += [f"{name}: {m['id']}: {p}" for m in document["mods"] for p in settings_problems(m) + floor_problems(m)]
    return document


def check_v3(index_path, problems):
    """A schema-3 index and its local detail files. Returns the schema-2 catalog they stand for, or None."""
    schema = json.loads((ROOT / "schemas" / "catalog-v3.schema.json").read_text(encoding="utf-8"))
    folder, name = index_path.parent, f"{index_path.parent.name}/{index_path.name}"
    try:
        index = read_strict(index_path, CATALOG_MAX_BYTES)
    except (OSError, ValueError) as error:
        problems.append(f"{name}: {error}")
        return None
    if errors := schema_errors(index, schema, schema):
        problems += [f"{name}: {e}" for e in errors]
        return None
    ids = [c["id"] for c in index["mods"]]
    problems += [f"{name}: id {i} listed more than once" for i in sorted(set(ids)) if ids.count(i) > 1]
    mods, listed = [], set()
    for card in index["mods"]:
        where = f"{name}: {card['id']}"
        if card["path"] != f"{card['id']}.json" or card["id"] == Path(INDEX_NAME).stem:
            problems.append(f"{where}: path must be {card['id']}.json (and no mod is called {Path(INDEX_NAME).stem})")
            continue
        listed.add(card["path"])
        detail_path = folder / card["path"]
        try:
            raw = detail_path.read_bytes()
            detail = read_strict(detail_path, DETAIL_MAX_BYTES)
        except (OSError, ValueError) as error:
            problems.append(f"{where}: {card['path']}: {error}")
            continue
        if hashlib.sha256(raw).hexdigest() != card["sha256"] or len(raw) != card["bytes"]:
            problems.append(f"{where}: {card['path']} does not match its pin (run: python3 -I tools/infmods.py catalog)")
        if errors := schema_errors(detail, schema["$defs"]["mod"], schema):
            problems += [f"{where}: {card['path']}: {e}" for e in errors]
            continue
        if (detail["id"], detail["version"]) != (card["id"], card["version"]):
            problems.append(f"{where}: {card['path']} says {detail['id']} {detail['version']}, the index {card['id']} {card['version']}")
            continue
        entry = entry_of(card, detail)
        if problem := layout_problem(entry):
            problems.append(f"{where}: {problem}")
        problems += [f"{where}: {p}" for p in settings_problems(entry) + floor_problems(entry)]
        mods.append(entry)
    problems += [f"{folder.name}/{p.name}: not listed in {name}" for p in sorted(folder.glob("*.json"))
                 if p.name != INDEX_NAME and p.name not in listed]
    return {"schemaVersion": 2, "mods": mods}


def check(catalog_path):
    """The official catalog (catalog/index.json and its detail files), or any one catalog file."""
    problems, catalog_path = [], Path(catalog_path)
    try:
        schema_version = json.loads(catalog_path.read_text(encoding="utf-8")).get("schemaVersion")
    except (OSError, ValueError) as error:
        print("FAIL", f"{catalog_path}: {error}")
        return 1
    if schema_version != 3:  # a schema-2 file: a custom catalog
        catalog = check_v2(catalog_path, catalog_path.name, problems)
    else:
        catalog = check_v3(catalog_path, problems)
    if (ROOT / "catalog.json").exists():
        problems.append("catalog.json: retired (every launcher reads catalog/index.json); delete it")
    for p in problems:
        print("FAIL", p)
    if not problems:
        print(f"ok: {catalog_path.name} {len(catalog['mods'])} mods")
    return 1 if problems else 0


PACK_SETS = {
    # set folder name (the external source's hint, the launcher's group key) -> source game
    "ZS FM5 Addon Packs": "5",
    "ZS FM6 Addon Packs": "6",
}
PACK_FILES = re.compile(r"^(mod\.json|database\.json|strings\.json|media/.+)$")
PACK_LEFTOVERS = re.compile(r"^database\.json\..+$")  # the author's backups (before_fh3overlay, before_racefix, ...)
PACK_MAKES = {"POR": "Porsche", "Mc Laren": "McLaren"}


def pack_cars(cars_path):
    """Car names per pack id from the server's fh2-packs.json (OD-231)."""
    cars = {}
    for car in json.loads(Path(cars_path).read_text(encoding="utf-8"))["cars"]:
        make = PACK_MAKES.get(car["make"], car["make"])
        name = f"{car['year']} {make} {car['model']}"
        cars.setdefault(car["pack"]["id"], []).append((car["pack"]["version"], name))
    return cars


def read_pack(zip_path):
    """One pack ZIP: its top folder, ExtraOptions mod.json and every file it maps, hashed."""
    with zipfile.ZipFile(zip_path) as archive:
        names = [i for i in archive.infolist() if not i.is_dir()]
        tops = {i.filename.split("/", 1)[0] for i in names}
        if len(tops) != 1 or any("/" not in i.filename for i in names):
            sys.exit(f"{zip_path.name}: not one top folder")
        top = tops.pop()
        manifest = json.loads(archive.read(f"{top}/mod.json").decode("utf-8"))
        if manifest.get("format_version") != 1 or not re.fullmatch(r"[a-z][a-z0-9-]{0,63}", manifest.get("id", "")):
            sys.exit(f"{zip_path.name}: mod.json is not an ExtraOptions format 1 manifest")
        files, skipped = [], []
        for info in names:
            relative = info.filename[len(top) + 1:]
            if PACK_LEFTOVERS.match(relative):
                skipped.append(relative)
                continue
            if not PACK_FILES.match(relative) or ".." in relative.split("/"):
                sys.exit(f"{zip_path.name}: unexpected file {info.filename}")
            digest = hashlib.sha256()
            with archive.open(info) as stream:
                for block in iter(lambda: stream.read(1 << 20), b""):
                    digest.update(block)
            files.append((relative, digest.hexdigest(), info.file_size))
    return top, manifest, sorted(files), skipped


def pack_entry(set_name, game, top, manifest, files, versions, cars, azimuth_versions, first):
    mod_id, version = manifest["id"], manifest["version"]
    common = next((d for d in manifest.get("dependencies", [])), None)
    if first:
        description = {
            "en": f"What every ZS FM{game} pack needs: shared database rows, manufacturer logos{', engine sounds' if game == '6' else ''} "
                  f"and its changes to FH2's own cars. Infinity keeps it first in the load order, before every other mod.",
            "pt-BR": f"O que todo pacote ZS FM{game} precisa: linhas compartilhadas do banco de dados, logotipos das marcas"
                     f"{', sons de motor' if game == '6' else ''} e as mudanças dele nos carros do próprio FH2. O Infinity o mantém "
                     f"primeiro na ordem de carregamento, antes de qualquer outro mod.",
        }
    else:
        names = sorted(name for pinned, name in cars.get(mod_id, []) if pinned == version)
        if not names:
            sys.exit(f"{mod_id} {version}: no cars in fh2-packs.json for this version")
        listed = "; ".join(names)
        description = {
            "en": f"{len(names)} car{'s' if len(names) > 1 else ''} from Forza Motorsport {game}, converted by ZephyrSkies: {listed}. "
                  f"Needs ZS FM{game} Common. Online, everyone in a lobby needs the same packs.",
            "pt-BR": f"{len(names)} carro{'s' if len(names) > 1 else ''} do Forza Motorsport {game}, convertidos por ZephyrSkies: {listed}. "
                     f"Precisa do ZS FM{game} Common. Online, todos no lobby precisam dos mesmos pacotes.",
        }
    install = {
        "schemaVersion": 1,
        "kind": "mod",
        "license": "Unknown. Player-supplied files; Infinity never distributes them.",
        "runtime": "standalone",
        "review": {"status": "reviewed", "blockers": []},
        "compatibility": {"azimuthVersions": azimuth_versions, "gameBuilds": [], "verification": "unverified"},
        "dependencies": [{"id": d, "versions": [versions[d]]} for d in manifest.get("dependencies", [])],
        "conflicts": [],
        "files": {
            "game": [],
            "azimuth": [{"source": f"pack:{top}/{relative}", "destination": f"{mod_folder(manifest)}{relative}",
                         "sha256": digest, "bytes": size, "role": "asset", "mode": "add-or-replace"}
                        for relative, digest, size in files],
        },
        "config": None,
        "runtimeOutputs": [],
        "excludedArchiveEntries": [],
        "databaseMigrations": [],
        "multiplayer": {"schemaVersion": 1, "impact": "gameplay", "compatibilityId": mod_id, "revision": 1,
                        "match": "exact", "settings": []},
        "countsAsModded": False,  # OD-236: the car packs never make a session modded; Hardcore keeps them
    }
    if first:
        install["loadOrder"] = "first"  # OD-232: the set's Common sits at the top of loadorder.txt
    if common and common not in versions:
        sys.exit(f"{mod_id}: depends on {common}, which is not in {set_name}")
    return {
        "id": mod_id,
        "version": version,
        "name": {"en": manifest["name"], "pt-BR": manifest["name"]},
        "description": description,
        "author": manifest.get("author") or "ZephyrSkies",
        "externalSources": [{
            "id": "pack",
            "kind": "either",
            "label": {"en": manifest["name"], "pt-BR": manifest["name"]},
            "help": {
                "en": f"Choose the folder with your {set_name} ZIPs (or any folder above it), or this pack's own ZIP or folder. "
                      f"Infinity checks every file and copies what it needs; you can move or delete the packs afterwards.",
                "pt-BR": f"Escolha a pasta com os ZIPs de {set_name} (ou qualquer pasta acima dela), ou o próprio ZIP ou pasta "
                         f"deste pacote. O Infinity confere cada arquivo e copia o que precisa; depois você pode mover ou apagar os pacotes.",
            },
            "hint": set_name,  # the same on every pack of the set: one pick serves the whole set
        }],
        "install": install,
    }


def external_packs(set_dirs, cars_path, catalog_path, out):
    """OD-231/OD-232: catalog entries for the ZephyrSkies pack sets, from the player-side ZIPs (read only)."""
    catalog = load_catalog(catalog_path)
    nitro = next((m for m in catalog["mods"] if m["id"] == "nitrosystem"), None)
    azimuth_versions = nitro["install"]["compatibility"]["azimuthVersions"] if nitro else ["1.0.4"]
    cars = pack_cars(cars_path)
    entries, report = [], []
    for set_dir in set_dirs:
        set_dir = Path(set_dir)
        game = PACK_SETS.get(set_dir.name) or sys.exit(f"{set_dir.name}: not a known pack set {sorted(PACK_SETS)}")
        packs = [read_pack(z) for z in sorted(set_dir.glob("*.zip"))]
        versions = {m["id"]: m["version"] for _, m, _, _ in packs}
        if len(versions) != len(packs):
            sys.exit(f"{set_dir.name}: repeated mod ids")
        bases = {d for _, m, _, _ in packs for d in m.get("dependencies", [])}
        ordered = sorted(packs, key=lambda p: (p[1]["id"] not in bases, p[1]["name"].lower()))
        for top, manifest, files, skipped in ordered:
            first = manifest["id"] in bases
            if first and manifest.get("dependencies"):
                sys.exit(f"{manifest['id']}: a Common with dependencies of its own")
            entries.append(pack_entry(set_dir.name, game, top, manifest, files, versions, cars, azimuth_versions, first))
            report.append(f"{manifest['id']} {manifest['version']}: {len(files)} files"
                          + (", load order first" if first else "") + (f", left out {', '.join(skipped)}" if skipped else ""))
    ids = {e["id"] for e in entries}
    catalog["mods"] = [m for m in catalog["mods"] if m["id"] not in ids] + entries
    index_bytes = write_catalog(catalog, Path(out))
    print("\n".join(report))
    print(f"{len(entries)} entries; index {index_bytes} bytes of {CATALOG_MAX_BYTES} -> {out}/catalog/")
    return 0


def pin_author(name, zip_path, write):
    """An authorPackage mod: pins its download (the author's release ZIP) and every file it installs from that ZIP,
    checks the author's mod.json (id, version, dependencies) and DLL (FH2PluginInitialize, allowed imports)."""
    folder, recipe, manifest = load_mod(name)
    author = recipe.get("authorPackage") or sys.exit(f"{name}: not an authorPackage mod")
    url = f"https://github.com/{author['repository']}/releases/download/{author['tag']}/{author['asset']}"
    data = Path(zip_path).read_bytes() if zip_path else download(url)
    problems = layout_problems(recipe, manifest)
    filled = copy.deepcopy(manifest)
    filled["package"] = {"url": url, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data), "format": "zip"}
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        names = set(archive.namelist())
        for file in filled["files"]["azimuth"]:
            if file["source"] not in names:
                problems.append(f"{file['source']} is not in {author['asset']}")
                continue
            body = archive.read(file["source"])
            file["sha256"], file["bytes"] = hashlib.sha256(body).hexdigest(), len(body)
            if file["destination"].endswith("/mod.json"):
                own = json.loads(body.decode("utf-8-sig"))
                wanted = {d["id"] for d in manifest.get("dependencies", []) if d["id"] != "extraoptions"}
                if own.get("format_version") != 1 or own.get("id") != manifest["id"] or own.get("version") != manifest["version"]:
                    problems.append(f"the author's mod.json is {own.get('id')} {own.get('version')}: the manifest must say the same")
                if set(own.get("dependencies", [])) != wanted:
                    problems.append("the author's mod.json declares other dependencies than the manifest")
            if file["role"] == "binary":
                machine, exports, imports = pe_report_bytes(body)
                if "FH2PluginInitialize" not in exports:
                    problems.append(f"{file['source']}: FH2PluginInitialize not exported")
                if extra := sorted({i.upper() for i in imports} - {i.upper() for i in recipe.get("allowedImports", [])}):
                    problems.append(f"{file['source']}: imports {extra} beyond allowedImports")
    changed = filled != manifest
    for p in problems:
        print("FAIL", p)
    print(f"{name}: {url} sha256 {filled['package']['sha256']} ({len(data)} bytes)"
          + (": pins changed" if changed else ": already pinned"))
    if problems:
        return 1
    if changed and write:
        (folder / "manifest.json").write_text(json.dumps(filled, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
        print(f"wrote mods/{name}/manifest.json; then release (or catalog --from) carries it into catalog/")
    return 0


ADAPTER_SOURCE = ROOT / "adapters" / "fh2-mod-platform-eo" / "fh2_mod_platform_eo_adapter.cpp"
UPSTREAM_ZIP_MAX = 256 * 1024 * 1024
SEMVER = re.compile(r"\d+\.\d+\.\d+")


def github(path):
    """One GitHub REST read (public, no credentials unless GITHUB_TOKEN is set), as JSON."""
    import urllib.request
    request = urllib.request.Request("https://api.github.com/" + path, headers={
        "Accept": "application/vnd.github+json", "User-Agent": "infmods",
        **({"Authorization": "Bearer " + os.environ["GITHUB_TOKEN"]} if os.environ.get("GITHUB_TOKEN") else {})})
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.loads(response.read())


def download(url):
    import urllib.request
    if not url.startswith("https://"):
        sys.exit(f"refusing a non-HTTPS download: {url}")
    request = urllib.request.Request(url, headers={"User-Agent": "infmods"})
    with urllib.request.urlopen(request, timeout=300) as response:
        data = response.read(UPSTREAM_ZIP_MAX + 1)
    if len(data) > UPSTREAM_ZIP_MAX:
        sys.exit(f"{url}: larger than {UPSTREAM_ZIP_MAX} bytes")
    return data


def sync(name, tag, zip_path, url, write):
    """Re-pin a mod that runs the original author's release DLL through the FH2 Mod Platform adapter to an upstream
    release (`upstreamRelease` in its recipe): the release ZIP's URL, SHA-256 and size, every file the manifest takes from
    it (matched by the recipe's patterns), excludedArchiveEntries, and the version (<upstream>-infinity.1, or the next
    -infinity.<n> when the upstream version is unchanged but its ZIP is not) in mod.json, manifest.json and the package's
    ExtraOptions mod.json. Checks the DLL (x64, fh2mod_init, imports), the upstream submodule against the tag's commit,
    the platform API the adapter implements and the INI keys the mod reads against the manifest's settings form.
    Without --write it only reports. Nothing is built, committed or published."""
    folder, recipe, manifest = load_mod(name)
    spec = recipe.get("upstreamRelease") or sys.exit(f"{name}: the recipe has no upstreamRelease")
    problems, notes = [], []
    if zip_path:
        if not (url and tag):
            sys.exit("--zip needs --url and --tag")
        data = Path(zip_path).read_bytes()
    else:
        release_info = github(f"repos/{spec['repository']}/releases/" + (f"tags/{tag}" if tag else "latest"))
        tag = release_info["tag_name"]
        assets = [a for a in release_info.get("assets", []) if re.fullmatch(spec["asset"], a["name"])]
        if len(assets) != 1:
            sys.exit(f"{name}: release {tag} has {len(assets)} assets matching {spec['asset']}")
        url = assets[0]["browser_download_url"]
        data = download(url)
    version = tag[1:] if tag[:1] in "vV" else tag
    if not SEMVER.fullmatch(version):
        sys.exit(f"{name}: tag {tag} is not <a.b.c>")
    digest = hashlib.sha256(data).hexdigest()
    package = manifest.get("additionalPackages", {}).get("upstream-assets", {})
    if not re.fullmatch(r"\d+\.\d+\.\d+-infinity\.\d+", manifest["version"]):
        sys.exit(f"{name}: version {manifest['version']} is not <a.b.c>-infinity.<n>")
    old_upstream = manifest["version"].split("-infinity.")[0]
    if package.get("sha256") == digest:
        print(f"{name}: already pinned to {url} ({digest}); nothing to do")
        return 0
    if tuple(map(int, version.split("."))) < tuple(map(int, old_upstream.split("."))):
        sys.exit(f"{name}: {tag} is older than the pinned {old_upstream}")
    revision = int(manifest["version"].split("-infinity.")[1]) + 1 if old_upstream == version else 1
    new_version = f"{version}-infinity.{revision}"

    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        entries = {i.filename: archive.read(i) for i in archive.infolist() if not i.is_dir()}
    taken = {}
    for pattern, destination in spec["files"].items():
        matches = [e for e in entries if re.fullmatch(pattern, e)]
        if len(matches) != 1:
            problems.append(f"{len(matches)} entries match {pattern} (for {destination}): {matches}")
            continue
        taken[mod_folder(manifest) + destination] = matches[0]
    mod_dll = mod_folder(manifest) + recipe["adapterConfig"]["modDll"]
    if mod_dll in taken:
        try:
            machine, exports, imports = pe_report_bytes(entries[taken[mod_dll]])
            if machine != 0x8664:
                problems.append("the mod's DLL is not x64")
            if "fh2mod_init" not in exports:
                problems.append("the mod's DLL does not export fh2mod_init (not an FH2 Mod Platform mod any more?)")
            allowed = {d.lower() for d in spec["allowedImports"]}
            if extra := [d for d in imports if d.lower() not in allowed]:
                problems.append(f"the mod's DLL imports {extra}, not in upstreamRelease.allowedImports")
            notes.append(f"mod DLL exports {exports}, imports {imports}")
        except (ValueError, struct.error, UnicodeDecodeError) as error:
            problems.append(f"the mod's DLL is not a PE32+ file: {error}")
    else:
        problems.append(f"no pattern maps adapterConfig.modDll ({recipe['adapterConfig']['modDll']})")

    # The submodule must be the tag's commit: its platform header is what the adapter is built against.
    submodule = ROOT / recipe["upstream"]["submodule"]
    try:
        tag_commit = github(f"repos/{spec['repository']}/commits/{tag}")["sha"]
        head = git("-C", str(submodule), "rev-parse", "HEAD") if (submodule / ".git").exists() else "(not checked out)"
        if head != tag_commit:
            problems.append(f"{recipe['upstream']['submodule']} is at {head}, the tag {tag} is {tag_commit}: "
                            f"git -C {recipe['upstream']['submodule']} fetch --tags && git -C {recipe['upstream']['submodule']} checkout {tag_commit}")
    except (OSError, subprocess.CalledProcessError) as error:
        problems.append(f"could not resolve {tag}'s commit: {error}")
    header = next(iter(sorted(submodule.glob("platform/include/fh2mod.h"))), None)
    supported = re.search(r"FH2MOD_API_VERSION == (\d+)", ADAPTER_SOURCE.read_text(encoding="utf-8")).group(1)
    if header:
        api = re.search(r"#define FH2MOD_API_VERSION (\d+)", header.read_text(encoding="utf-8", errors="replace"))
        if not api or api.group(1) != supported:
            problems.append(f"the mod is built against FH2 Mod Platform API {api.group(1) if api else '?'}, the adapter "
                            f"implements {supported}: update adapters/fh2-mod-platform-eo first")
    else:
        problems.append(f"no platform/include/fh2mod.h in {recipe['upstream']['submodule']}")

    # INI keys the mod reads (its sources) against the manifest's settings form. A form key whose name is nowhere in
    # the sources is gone; keys read through GetPrivateProfile*W or the mod's ini_* helpers that the form lacks are
    # listed for review (keys kept in tables, such as FrontEffect, are found by name only).
    sources = "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in sorted(submodule.glob("src/**/*"))
                        if p.suffix.lower() in (".c", ".cpp", ".h", ".hpp"))
    read = set(re.findall(r'(?:GetPrivateProfile(?:String|Int)W|ini_\w+)\(\s*L"(\w+)"\s*,\s*L"(\w+)"', sources))
    form = {(o["section"], o["key"]) for o in (manifest.get("config") or {}).get("options", [])}
    if gone := sorted(k for k in form if f'"{k[1]}"' not in sources):
        problems.append(f"settings in the manifest whose key is nowhere in the mod's sources: {gone}")
    if new := sorted(k for k in read - form if k[0] in {s for s, _ in form}):
        notes.append(f"INI keys the mod reads that the settings form does not offer (review): {new}")

    filled = copy.deepcopy(manifest)
    filled["version"] = new_version
    filled["additionalPackages"]["upstream-assets"] = {"url": url, "sha256": digest, "bytes": len(data), "format": "zip"}
    for file in filled["files"]["azimuth"]:
        if file.get("package") != "upstream-assets":
            continue
        if file["destination"] not in taken:
            problems.append(f"{file['destination']} comes from the original release but no recipe pattern maps it")
            continue
        content = entries[taken[file["destination"]]]
        file["source"], file["sha256"], file["bytes"] = taken[file["destination"]], hashlib.sha256(content).hexdigest(), len(content)
    filled["excludedArchiveEntries"] = sorted(set(entries) - set(taken.values()))
    filled["package"] = {"url": "", "sha256": "", "bytes": 0, "format": "zip"}
    filled["source"]["commit"] = ""
    print(f"{name}: {manifest['version']} -> {new_version} from {url}\n  sha256 {digest}, {len(data)} bytes")
    for n in notes:
        print("  note:", n)
    for p in problems:
        print("  FAIL", p)
    if problems:
        return 1
    if not write:
        print("  (report only; --write updates mod.json, manifest.json and extraoptions/mod.json)")
        return 0
    recipe["version"] = new_version
    recipe["upstream"]["version"] = version
    own_path = ROOT / recipe["package"]["mod.json"]
    own = json.loads(own_path.read_text(encoding="utf-8"))
    own["version"] = new_version
    for path, value in ((folder / "mod.json", recipe), (folder / "manifest.json", filled), (own_path, own)):
        path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    print("  written: mod.json, manifest.json, " + recipe["package"]["mod.json"])
    return 0


def pe_report_bytes(data):
    with tempfile.TemporaryDirectory(prefix="infmods-pe-") as temporary:
        path = Path(temporary) / "module.dll"
        path.write_bytes(data)
        return pe_report(path)


# No commit message that leaves this repository may name an assistant conversation or session:
# no `Claude-Session:` line, claude.ai conversation, session or share link, or `session_<id>`.
SESSION_PATTERN = re.compile(
    r"Claude-Session:|claude\.ai/(?:code|chat|share|project)s?/|\bsession_[A-Za-z0-9]{16,}", re.I)


def messages(revs):
    """Fail when a commit in `revs` (git rev-list arguments) names an assistant session."""
    # shutil.which finds a git.cmd shim too, which CreateProcess would not resolve by the bare name.
    git = shutil.which("git") or sys.exit("git not found")
    log = subprocess.run([git, "-C", str(ROOT), "log", "--format=%H%x00%B%x01", *revs],
                         check=True, capture_output=True, text=True).stdout
    bad = [sha for sha, _, body in (e.strip().partition("\0") for e in log.split("\x01"))
           if sha and SESSION_PATTERN.search(body)]
    for sha in bad:
        print(f"messages: commit {sha[:8]} names an assistant session or conversation; reword it before pushing")
    print(f"messages: {len(bad)} problem(s)")
    return 1 if bad else 0


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
    r.add_argument("--catalog", type=Path, default=CATALOG_DIR / INDEX_NAME, help="the current catalog: catalog/index.json, or a v2 file (read only)")
    r.add_argument("--out", type=Path, default=ROOT / "dist")
    r.add_argument("--allow-dirty", action="store_true", help="development only: the commit pinned is HEAD, not the tree")
    m = sub.add_parser("messages", help="refuse commit messages that name an assistant session (before every push)")
    m.add_argument("revs", nargs="*", default=["origin/main..HEAD"], help="git rev-list arguments (default: origin/main..HEAD)")
    c = sub.add_parser("check")
    c.add_argument("--catalog", type=Path, default=CATALOG_DIR / INDEX_NAME)
    g = sub.add_parser("catalog", help="re-pin catalog/index.json after editing catalog/")
    g.add_argument("--from", dest="from_v2", type=Path, help="split this schema-2 catalog into catalog/ instead")
    e = sub.add_parser("external-packs", help="catalog entries for the ZephyrSkies pack sets from their ZIPs (read only)")
    e.add_argument("sets", nargs="+", type=Path, help="a set folder of pack ZIPs, named as in PACK_SETS")
    e.add_argument("--cars", type=Path, required=True, help="the monorepo's crates/fi-gamedb/data/fh2-packs.json (OD-231)")
    e.add_argument("--catalog", type=Path, default=CATALOG_DIR / INDEX_NAME)
    e.add_argument("--out", type=Path, required=True, help="a folder: gets catalog/")
    a = sub.add_parser("pin-author", help="re-pin an authorPackage mod to its author's release ZIP (report; --write to apply)")
    a.add_argument("mod")
    a.add_argument("--zip", type=Path, help="the author's ZIP, already downloaded from its release URL")
    a.add_argument("--write", action="store_true")
    s = sub.add_parser("sync", help="re-pin an FH2 Mod Platform mod to an upstream release (report; --write to apply)")
    s.add_argument("mod")
    s.add_argument("--tag", help="the upstream release tag (default: its latest release)")
    s.add_argument("--zip", type=Path, help="a release ZIP already downloaded (needs --url and --tag)")
    s.add_argument("--url", help="where --zip was downloaded from (the URL the manifest pins)")
    s.add_argument("--write", action="store_true")
    args = parser.parse_args()
    if args.cmd == "messages":
        return messages(args.revs)
    if args.cmd == "check":
        return check(args.catalog)
    if args.cmd == "catalog":
        return catalog_command(args.from_v2)
    if args.cmd == "pin-author":
        return pin_author(args.mod, args.zip, args.write)
    if args.cmd == "sync":
        return sync(args.mod, args.tag, args.zip, args.url, args.write)
    if args.cmd == "external-packs":
        return external_packs(args.sets, args.cars, args.catalog, args.out)
    zig = args.zig.resolve() if args.zig else None
    if zig:
        zig_check(zig)
    if args.cmd == "build":
        print(build(args.mod, zig, args.out.resolve()))
        _, recipe, _ = load_mod(args.mod)
        if recipe.get("modBuild"):
            print(build_mod(recipe, args.out.resolve() / "mod"))
        return 0
    if args.cmd == "verify":
        return verify(args.mod, zig)
    return release(args.mods, zig, args.tag, args.catalog, args.out.resolve(), args.allow_dirty)


if __name__ == "__main__":
    sys.exit(main())
