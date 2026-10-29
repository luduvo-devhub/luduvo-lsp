"""Exercise production autodumping with a real editor and an isolated project."""

from __future__ import annotations

import argparse
import io
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import tempfile
import time
import zipfile

from dumpLuduvoResources import RELEASE_CHANNEL_URL, release_manifest
from dumpLuduvoTypes import fetch_bytes


def stop_editor(process: subprocess.Popen) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(process.pid), "/T", "/F"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    else:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        if os.name != "nt":
            os.killpg(process.pid, signal.SIGKILL)
        else:
            process.kill()
        process.wait(timeout=5)


def bootstrap_project(editor: Path, project: Path, directory: Path, env: dict) -> None:
    project.mkdir()
    (project / "project.json").write_text(
        json.dumps({"name": "Luduvo LSP Smoke", "assets": {}}), encoding="utf-8"
    )
    dump = directory / "bootstrap.txt"
    arguments = [str(editor), str(project)]
    if platform.system() == "Windows":
        arguments += ["-platform", "offscreen"]
    for command in ("insert.part", "file.save", f"world.dump {dump.as_posix()}"):
        arguments += ["--exec", command]
    with (directory / "editor-bootstrap.log").open("wb") as log:
        process = subprocess.Popen(
            arguments,
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=os.name != "nt",
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
        try:
            deadline = time.monotonic() + 120
            previous = None
            while time.monotonic() < deadline:
                if dump.exists() and (project / "world.ldv").exists():
                    contents = dump.read_bytes()
                    if contents == previous and b" name Part " in contents:
                        return
                    previous = contents
                if process.poll() is not None:
                    raise RuntimeError(f"Editor bootstrap exited with code {process.returncode}")
                time.sleep(0.2)
            raise RuntimeError("Editor bootstrap did not save and dump the fixture project within 120 seconds")
        finally:
            stop_editor(process)


def run_smoke(runner: Path, root: Path, existing_editor_root: Path | None) -> None:
    env = os.environ.copy()
    # Isolate Linux editor state. Windows known-folder APIs may still use the
    # installed user profile despite APPDATA/LOCALAPPDATA overrides.
    for variable, subdirectory in {
        "XDG_DATA_HOME": "data",
        "XDG_CACHE_HOME": "cache",
        "XDG_CONFIG_HOME": "config",
        "XDG_STATE_HOME": "state",
        "APPDATA": "roaming",
        "LOCALAPPDATA": "local",
    }.items():
        path = root / subdirectory
        path.mkdir()
        env[variable] = str(path)
    if os.name != "nt":
        home = root / "home"
        home.mkdir()
        env["HOME"] = str(home)

    if existing_editor_root:
        editor_root = existing_editor_root.resolve()
        relative_editor = "LuduvoEditor.exe" if platform.system() == "Windows" else "usr/bin/LuduvoEditor"
    else:
        key = {"Linux": "linux-x86_64", "Windows": "windows-x86_64", "Darwin": "macos-arm64"}.get(platform.system())
        if not key:
            raise RuntimeError("The smoke-test downloader supports Linux, Windows and macOS bundles")
        channel = RELEASE_CHANNEL_URL.rsplit("/", 1)[0] + f"/{key}.txt"
        manifest = release_manifest(channel)
        package = manifest["platforms"][key]
        relative_editor = package["editor_exe"]
        editor_root = root / "install"
        (root / "release.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        print(f"Downloading real Luduvo {manifest['version']} bundle: {package['bundle']['url']}", flush=True)
        with zipfile.ZipFile(io.BytesIO(fetch_bytes(package["bundle"]["url"]))) as bundle:
            for entry in bundle.infolist():
                extracted = Path(bundle.extract(entry, editor_root / "bundle"))
                permissions = (entry.external_attr >> 16) & 0o777
                if permissions:
                    extracted.chmod(permissions)

    editor = editor_root / "bundle" / relative_editor
    project = root / "project"
    bootstrap_project(editor, project, root, env)

    # Exercise the real recovery-prompt prevention as well as editor startup.
    world = project / "world.ldv"
    autosave = project / "world.ldv.autosave"
    original = world.read_bytes()
    autosave.write_bytes(original)
    newer = max(time.time(), world.stat().st_mtime + 2)
    os.utime(autosave, (newer, newer))
    with (root / "autodump.log").open("wb") as log:
        subprocess.run(
            [str(runner), str(editor_root), str(project), str(root / "world.dump.txt")],
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            timeout=90,
            check=True,
        )
    if autosave.read_bytes() != original or world.read_bytes() != original:
        raise RuntimeError("Autodumping changed the saved fixture world or its autosave")
    print((root / "autodump.log").read_text(encoding="utf-8"), end="", flush=True)
    print(f"Smoke-test artifacts: {root}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", type=Path, required=True, help="Built luduvo-context-editor-smoke executable")
    parser.add_argument("--editor-root", type=Path, help="Existing Luduvo install root; otherwise download the current bundle")
    parser.add_argument("--work-directory", type=Path, help="Empty directory for isolated fixtures, editor data and diagnostic logs")
    arguments = parser.parse_args()
    root = (arguments.work_directory or Path(tempfile.mkdtemp(prefix="luduvo-editor-smoke-"))).resolve()
    root.mkdir(parents=True, exist_ok=True)
    if any(root.iterdir()):
        parser.error("--work-directory must be empty")
    try:
        run_smoke(arguments.runner.resolve(), root, arguments.editor_root)
        return 0
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        print(f"Real-editor smoke test failed: {error}\nDiagnostic artifacts: {root}", flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
