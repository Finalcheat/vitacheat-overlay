#!/usr/bin/env python3
"""Build and verify complete VitaCheat Overlay installation packages."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
from zipfile import BadZipFile, ZIP_DEFLATED, ZipFile, ZipInfo

ROOT = Path(__file__).resolve().parents[1]
PLUGINS = ("VitaCheatOverlayKernel.skprx", "VitaCheatOverlayShell.suprx",
           "VitaCheatOverlayUI.suprx", "VitaCheatOverlayGame.suprx")
DOCUMENTS = ("README.md", "README.zh-CN.md", "LICENSE", "CHANGELOG.md", "THIRD_PARTY_NOTICES.md",
             "CONTRIBUTING.md", "docs/ARCHITECTURE.md", "docs/CODE_FORMATS.md",
             "docs/BUILDING.md", "docs/TESTING.md", "docs/RELEASING.md",
             "docs/screenshots/overlay-en.png", "docs/screenshots/overlay-zh-CN.png")
NOTICES = ("vendor/taihen/LICENSE", "vendor/unicode/LICENSE.txt",
           "resources/PSVshellPlus-LICENSE")


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def version() -> str:
    return re.search(r'#define VCO_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"',
                     (ROOT / "src/version.h").read_text()).group(1)


def source_identity() -> dict:
    def git(*args):
        return subprocess.run(["git", *args], cwd=ROOT, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    top = git("rev-parse", "--show-toplevel")
    if top.returncode or Path(top.stdout.strip()).resolve() != ROOT.resolve():
        return {"commit": None, "dirty": None}
    commit = git("rev-parse", "HEAD")
    status = git("status", "--porcelain", "--untracked-files=no")
    if commit.returncode or status.returncode:
        raise ValueError("Could not determine Git source identity")
    return {"commit": commit.stdout.strip(), "dirty": bool(status.stdout.strip())}


def verified_build(harness: bool = False) -> dict:
    spec = importlib.util.spec_from_file_location("vco_build_stamp", ROOT / "scripts/build_stamp.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.ROOT = ROOT
    path = ROOT / ("build/harness-build.json" if harness else "build/plugin-build.json")
    current = module.harness() if harness else module.current()
    if not path.exists() or json.loads(path.read_text()) != current:
        script = "build-harness.sh" if harness else "build.sh"
        raise ValueError("Sources or artifacts changed; run scripts/" + script)
    return current


def payloads(include_harness: bool = False) -> dict[str, bytes]:
    record = verified_build()
    result = {}
    for name in (*PLUGINS, "vco-base.rco"):
        data = (ROOT / "build" / name).read_bytes()
        if digest(data) != record["artifacts"]["build/" + name]:
            raise ValueError("Artifact changed while packaging: " + name)
        if name in PLUGINS and not data.startswith(b"SCE\0"):
            raise ValueError("Not a Vita SELF: " + name)
        result["ux0/vitacheat/" + name] = data
    for name in (*DOCUMENTS, *NOTICES):
        result[name] = (ROOT / name).read_bytes()
    result["build-stamp.json"] = (json.dumps(record, indent=2, sort_keys=True) + "\n").encode()
    if include_harness:
        harness_record = verified_build(True)
        data = (ROOT / "build/harness/VcoDeviceHarness.vpk").read_bytes()
        if digest(data) != harness_record["artifacts"]["build/harness/VcoDeviceHarness.vpk"]:
            raise ValueError("Harness changed while packaging")
        result["tools/VcoDeviceHarness.vpk"] = data
        for name in ("PSPSDK-LICENSE.txt", "THIRD-PARTY.md"):
            result["tools/device-harness/" + name] = (ROOT / "tools/device-harness" / name).read_bytes()
        result["harness-build-stamp.json"] = (json.dumps(harness_record, indent=2, sort_keys=True) + "\n").encode()
    schema = int(re.search(r"#define VCO_CONFIG_VERSION (\d+)",
                           (ROOT / "src/config/config.h").read_text()).group(1))
    abi = re.search(r"#define VCO_AGENT_ABI (0x[0-9A-Fa-f]+)",
                    (ROOT / "src/agent/protocol.h").read_text()).group(1)
    manifest = {
        "schema": 1, "name": "VitaCheat Overlay", "version": version(),
        "variant": "source-build", "hardware_tested": False,
        "qualification": "Fresh source build; requires hardware validation before release.",
        "native_execution_connected": True, "execution_backend": "game_suprx",
        "game_plugin_required": True, "private_agent_abi": abi,
        "storage_root": "ux0:vitacheat", "settings_schema": schema,
        "component_versions": dict.fromkeys(("kernel", "loader", "ui", "game", "resource"), version()),
        "includes_harness": include_harness,
        "source": source_identity(),
        "files": {name: {"bytes": len(data), "sha256": digest(data)}
                  for name, data in sorted(result.items())},
    }
    result["manifest.json"] = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()
    return result


def verify_archive(path: Path, expected_version: str | None = None,
                   expected_commit: str | None = None) -> dict:
    checksum = path.with_suffix(path.suffix + ".sha256").read_text()
    if checksum != f"{digest(path.read_bytes())}  {path.name}\n":
        raise ValueError("Archive checksum mismatch")
    with ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise ValueError("Duplicate package paths")
        if any(name.startswith("/") or "\\" in name or
               any(part in ("", ".", "..") for part in name.split("/")) for name in names):
            raise ValueError("Unsafe package path")
        if sum(info.file_size for info in archive.infolist()) > 32 * 1024 * 1024:
            raise ValueError("Package exceeds size limit")
        manifest = json.loads(archive.read("manifest.json"))
        if not isinstance(manifest, dict):
            raise ValueError("Invalid package manifest")
        if (manifest.get("schema") != 1 or manifest.get("storage_root") != "ux0:vitacheat" or
                manifest.get("game_plugin_required") is not True or
                manifest.get("native_execution_connected") is not True or
                manifest.get("execution_backend") != "game_suprx"):
            raise ValueError("Unexpected package contract")
        if expected_version and manifest.get("version") != expected_version:
            raise ValueError("Package version mismatch")
        if expected_commit and manifest.get("source") != {"commit": expected_commit, "dirty": False}:
            raise ValueError("Package source commit mismatch or uncommitted changes")
        records = manifest["files"]
        if not isinstance(records, dict):
            raise ValueError("Invalid manifest file records")
        if set(names) != set(records) | {"manifest.json"}:
            raise ValueError("Manifest coverage mismatch")
        required = {"ux0/vitacheat/" + name for name in (*PLUGINS, "vco-base.rco")}
        required.update((*DOCUMENTS, *NOTICES, "build-stamp.json"))
        if not required.issubset(records):
            raise ValueError("Missing installation files or notices")
        stamp = json.loads(archive.read("build-stamp.json"))
        if stamp.get("configuration", {}).get("resource_profile") is not False:
            raise ValueError("Unexpected developer profile package")
        for name, record in records.items():
            data = archive.read(name)
            if record != {"bytes": len(data), "sha256": digest(data)}:
                raise ValueError("Package payload mismatch: " + name)
            if name.startswith("ux0/vitacheat/"):
                filename = name.rsplit("/", 1)[1]
                if stamp["artifacts"].get("build/" + filename) != digest(data):
                    raise ValueError("Build stamp mismatch: " + name)
                if filename in PLUGINS and not data.startswith(b"SCE\0"):
                    raise ValueError("Not a Vita SELF: " + name)
    return manifest


def write_archive(path: Path, files: dict[str, bytes]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    try:
        with ZipFile(temporary, "w", compression=ZIP_DEFLATED, compresslevel=9) as archive:
            for name, data in sorted(files.items()):
                entry = ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
                entry.compress_type = ZIP_DEFLATED
                entry.create_system = 3
                entry.external_attr = 0o100644 << 16
                archive.writestr(entry, data, compresslevel=9)
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--with-harness", action="store_true")
    parser.add_argument("--verify", type=Path, help="Verify an existing package and checksum")
    parser.add_argument("--expected-commit", help="Require a clean build of this Git commit")
    args = parser.parse_args()
    if args.verify:
        if args.output or args.with_harness:
            parser.error("--verify cannot be combined with packaging options")
        try:
            verify_archive(args.verify, version(), args.expected_commit)
        except (OSError, ValueError, KeyError, BadZipFile) as error:
            parser.exit(1, f"Verification failed: {error}\n")
        print(f"Verified: {args.verify}")
        return
    if args.expected_commit:
        parser.error("--expected-commit requires --verify")
    path = args.output or ROOT / f"dist/VitaCheatOverlay-{version()}.zip"
    try:
        files = payloads(args.with_harness)
        write_archive(path, files)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"Packaging failed: {error}\n")
    checksum = digest(path.read_bytes())
    path.with_suffix(path.suffix + ".sha256").write_text(f"{checksum}  {path.name}\n")
    print(f"Created: {path}\nSHA-256: {checksum}\nHardware qualification: pending")


if __name__ == "__main__":
    main()
