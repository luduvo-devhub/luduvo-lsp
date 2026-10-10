#!/usr/bin/env python3
# Run in root of repository
# scripts/release.py <major.minor.patch> [--prerelease]

import argparse
import json
import os
import re
import shutil
import subprocess
from datetime import datetime

CHANGELOG_FILE = "CHANGELOG.md"
MAKE_FILE = "CMakeLists.txt"
PACKAGE_JSON_FILE = "editors/code/package.json"
PACKAGE_LOCK_JSON_FILE = "editors/code/package-lock.json"
BASE_VERSION_PATTERN = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$"
) # This took me forever to get right. I hate regex


def find_command(name: str) -> str:
    command = f"{name}.cmd" if os.name == "nt" else name
    path = shutil.which(command)
    if path is None:
        raise RuntimeError(f"Could not find {command} on PATH")
    return path


def make_release_version(base_version: str, prerelease: bool) -> str:
    if not BASE_VERSION_PATTERN.fullmatch(base_version):
        raise ValueError(
            "Version must be major.minor.patch without anything else (like a leading v or L/alpha suffix)"
        )

    return f"{base_version}L{'-alpha' if prerelease else ''}"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Create and push a Luduvo LSP release commit and tag"
    )
    parser.add_argument(
        "version",
        help="numeric extension version in major.minor.patch form. Don't use any other parts of semver",
    )
    parser.add_argument(
        "--prerelease",
        action="store_true",
        help="append -alpha to the Luduvo release tag",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    npm = find_command("npm")
    npx = find_command("npx")
    try:
        release_version = make_release_version(args.version, args.prerelease)
    except ValueError as error:
        raise SystemExit(str(error)) from error

    changelog_date = datetime.now().strftime("%Y-%m-%d")

    # Version Bump Changelog
    new_changelog_lines: list[str] = []
    with open(CHANGELOG_FILE, "r") as file:
        lines = file.readlines()

        for line in lines:
            new_changelog_lines.append(line)
            if line.startswith("## [Unreleased]"):
                new_changelog_lines.append("\n")
                new_changelog_lines.append(
                    f"## [{release_version}] - {changelog_date}"
                )
                new_changelog_lines.append("\n")

    with open(CHANGELOG_FILE, "w") as file:
        file.writelines(new_changelog_lines)

    # Version Bump Makefile
    new_make_file_lines: list[str] = []
    with open(MAKE_FILE, "r") as file:
        for line in file:
            if line.startswith("set(LSP_VERSION"):
                new_make_file_lines.append(
                    f'set(LSP_VERSION "{release_version}")\n'
                )
            else:
                new_make_file_lines.append(line)

    with open(MAKE_FILE, "w") as file:
        file.writelines(new_make_file_lines)

    # Version Bump VSCode's package.json
    with open(PACKAGE_JSON_FILE, "r") as file:
        package_json_data = json.load(file)
        package_json_data["version"] = args.version

    with open(PACKAGE_JSON_FILE, "w") as file:
        json.dump(package_json_data, file)

    subprocess.run(
        [npm, "install", "--package-lock-only"], cwd="editors/code", check=True
    )
    subprocess.run(
        [npx, "prettier", "--write", CHANGELOG_FILE, PACKAGE_JSON_FILE],
        check=True,
    )

    # Create the new commit + tag
    subprocess.run(
        [
            "git",
            "add",
            CHANGELOG_FILE,
            MAKE_FILE,
            PACKAGE_JSON_FILE,
            PACKAGE_LOCK_JSON_FILE,
        ],
        check=True,
    )
    subprocess.run(["git", "commit", "-m", f"v{release_version}"], check=True)
    subprocess.run(
        ["git", "tag", "-a", release_version, "-m", release_version],
        check=True,
    )
    subprocess.run(["git", "push"], check=True)
    subprocess.run(["git", "push", "--tags"], check=True)


if __name__ == "__main__":
    main()
