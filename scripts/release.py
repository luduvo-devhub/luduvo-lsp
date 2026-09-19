#!/usr/bin/env python3
# Run in root of repository
# scripts/release.py <version number>

import sys
import json
import os
import shutil
import subprocess
from datetime import datetime

CHANGELOG_FILE = "CHANGELOG.md"
MAKE_FILE = "CMakeLists.txt"
PACKAGE_JSON_FILE = "editors/code/package.json"
PACKAGE_LOCK_JSON_FILE = "editors/code/package-lock.json"

assert len(sys.argv) == 2, "Usage: scripts/release.py <version number>"
VERSION = sys.argv[1]

CHANGELOG_DATE = datetime.now().strftime("%Y-%m-%d")


def find_command(name: str) -> str:
    command = f"{name}.cmd" if os.name == "nt" else name
    path = shutil.which(command)
    if path is None:
        raise RuntimeError(f"Could not find {command} on PATH")
    return path


NPM = find_command("npm")
NPX = find_command("npx")

# Update version in CHANGELOG.md
new_changelog_lines: list[str] = []
with open(CHANGELOG_FILE, "r") as file:
    lines = file.readlines()

    for line in lines:
        new_changelog_lines.append(line)
        if line.startswith("## [Unreleased]"):
            new_changelog_lines.append("\n")
            new_changelog_lines.append(f"## [{VERSION}] - {CHANGELOG_DATE}")
            new_changelog_lines.append("\n")

with open(CHANGELOG_FILE, "w") as file:
    file.writelines(new_changelog_lines)

# Update version in main.cpp
new_make_file_lines: list[str] = []
with open(MAKE_FILE, "r") as file:
    for line in file:
        if line.startswith("set(LSP_VERSION"):
            new_line = f"set(LSP_VERSION \"{VERSION}\")\n"
            new_make_file_lines.append(new_line)
        else:
            new_make_file_lines.append(line)

with open(MAKE_FILE, "w") as file:
    file.writelines(new_make_file_lines)

# Update version in package.json
package_json_data = None
with open(PACKAGE_JSON_FILE, "r") as t:
    package_json_data = json.load(t)
    package_json_data["version"] = VERSION

with open(PACKAGE_JSON_FILE, "w") as t:
    json.dump(package_json_data, t)

# Update lockfile
subprocess.run([NPM, "install", "--package-lock-only"], cwd="editors/code", check=True)

# Run prettier
subprocess.run(
    [NPX, "prettier", "--write", CHANGELOG_FILE, PACKAGE_JSON_FILE], check=True
)

# Commit
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
subprocess.run(["git", "commit", "-m", f"v{VERSION}"], check=True)

# Tag
subprocess.run(["git", "tag", "-a", VERSION, "-m", VERSION], check=True)

# Push
subprocess.run(["git", "push"], check=True)
subprocess.run(["git", "push", "--tags"], check=True)
