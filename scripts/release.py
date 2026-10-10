#!/usr/bin/env python3
"""Validate a release tag and prepare reviewed notes; no GitHub writes."""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TAG = re.compile(r"v((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))"
                 r"(?:-(alpha|beta|rc)\.(0|[1-9][0-9]*))?")


def check_tag(tag: str, root: Path = ROOT) -> dict:
    match = TAG.fullmatch(tag)
    if not match:
        raise ValueError("Expected vMAJOR.MINOR.PATCH, optionally -alpha.N, -beta.N, or -rc.N")
    source = (root / "src/version.h").read_text()
    version = re.search(r'#define VCO_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"', source)
    if not version or match.group(1) != version.group(1):
        raise ValueError("Release tag does not match src/version.h")
    def git(*args):
        result = subprocess.run(["git", *args], cwd=root, text=True, capture_output=True)
        if result.returncode:
            raise ValueError("Release tag is missing or does not resolve to a commit")
        return result.stdout.strip()
    commit = git("rev-parse", "--verify", f"refs/tags/{tag}^{{commit}}")
    if commit != git("rev-parse", "HEAD"):
        raise ValueError("Checkout differs from the release tag")
    notes = root / "docs/releases" / (version.group(1) + ".md")
    if not notes.exists() or not notes.read_text().strip():
        raise ValueError("Add reviewed bilingual notes at docs/releases/" + version.group(1) + ".md")
    return {"tag": tag, "version": version.group(1), "commit": commit,
            "prerelease": bool(match.group(2))}


def verify_assets(directory: Path, record: dict) -> Path:
    path = directory / f"VitaCheatOverlay-{record['version']}.zip"
    spec = importlib.util.spec_from_file_location("vco_package", ROOT / "scripts/package.py")
    package = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(package)
    package.verify_archive(path, record["version"], record["commit"])
    return path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("check", "prepare"))
    parser.add_argument("--tag", required=True)
    parser.add_argument("--github-output", type=Path)
    parser.add_argument("--assets", type=Path, default=ROOT / "dist")
    parser.add_argument("--notes-output", type=Path, default=ROOT / "release-notes.md")
    args = parser.parse_args()
    try:
        record = check_tag(args.tag)
        if args.action == "prepare":
            record["archive"] = str(verify_assets(args.assets, record))
            notes = (ROOT / "docs/releases" / (record["version"] + ".md")).read_text()
            if record["prerelease"]:
                notes = f"Pre-release: `{record['tag']}`\n\n" + notes
            args.notes_output.parent.mkdir(parents=True, exist_ok=True)
            args.notes_output.write_text(notes, encoding="utf-8")
        if args.github_output:
            with args.github_output.open("a", encoding="utf-8") as output:
                for name, value in record.items():
                    if name != "archive":
                        output.write(f"{name}={str(value).lower() if isinstance(value, bool) else value}\n")
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"Release validation failed: {error}\n")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
