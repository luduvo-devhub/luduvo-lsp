"""Copy Luduvo Studio's current Luau declarations into the LSP snapshots.

Luduvo's local content store or official content service is authoritative for
API types. The website catalog is deliberately not consulted here; it remains
a documentation-only source.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CONTENT_MANIFEST_URL = "https://api.luduvo.com/content/manifest"
CONTENT_BASE_URL = "https://assets.luduvo.com/content/"
USER_AGENT = "luduvo-lsp definitions dumper"
OUTPUTS = {
    "server": ROOT / "api-docs" / "en-us" / "globalTypes.luduvo.server.d.luau",
    "client": ROOT / "api-docs" / "en-us" / "globalTypes.luduvo.client.d.luau",
}
SURFACE_FILES = {
    "server": "defs/luduvo.d.luau",
    "client": "defs/luduvo.client.d.luau",
}
VERSION_NAME = re.compile(r"[A-Za-z0-9._-]+\Z")
CONTENT_HASH = re.compile(r"[0-9a-fA-F]{64}\Z")


class DefinitionsError(RuntimeError):
    """Raised when an installed declaration snapshot cannot be read."""


def default_data_directory() -> Path:
    override = os.environ.get("LUDUVO_DATA_DIRECTORY")
    if override:
        return Path(override)

    system = platform.system()
    if system == "Windows":
        roaming = os.environ.get("APPDATA")
        if not roaming:
            raise DefinitionsError("APPDATA is not set; pass --data-directory")
        return Path(roaming) / "Luduvo" / "Client"
    if system == "Darwin":
        return Path.home() / "Library" / "Application Support" / "Luduvo" / "Client"

    data_home = os.environ.get("XDG_DATA_HOME")
    base = Path(data_home) if data_home else Path.home() / ".local" / "share"
    return base / "Luduvo" / "Client"


def current_version(content_root: Path) -> str:
    path = content_root / "current"
    try:
        version = path.read_text(encoding="utf-8").strip()
    except OSError as error:
        raise DefinitionsError(
            f"could not read the current content pointer at {path}: {error}"
        ) from error

    if not version or not VERSION_NAME.fullmatch(version) or version in {".", ".."}:
        raise DefinitionsError(f"invalid current content version {version!r}")
    return version


def installed_definitions(content_root: Path, surface: str) -> tuple[Path, str, str]:
    version = current_version(content_root)
    path = content_root / "versions" / version / SURFACE_FILES[surface]
    try:
        return path, path.read_text(encoding="utf-8"), version
    except OSError as error:
        raise DefinitionsError(
            f"could not read the {surface} declarations at {path}: {error}"
        ) from error


def explicit_definitions(path: Path) -> tuple[Path, str, None]:
    resolved = path.expanduser().resolve()
    try:
        return resolved, resolved.read_text(encoding="utf-8"), None
    except OSError as error:
        raise DefinitionsError(
            f"could not read the declaration file at {resolved}: {error}"
        ) from error


def fetch_bytes(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.read()
    except OSError as error:
        raise DefinitionsError(f"could not download {url}: {error}") from error


def remote_definitions(
    manifest_url: str, content_base_url: str = CONTENT_BASE_URL
) -> dict[str, tuple[str, str, str]]:
    """Download both declaration surfaces named by Luduvo's content manifest."""
    try:
        manifest = json.loads(fetch_bytes(manifest_url))
        version = str(manifest["version"])
        files = manifest["files"]
    except (KeyError, TypeError, UnicodeError, json.JSONDecodeError) as error:
        raise DefinitionsError(
            f"content manifest at {manifest_url} has an invalid shape"
        ) from error

    if not isinstance(files, list):
        raise DefinitionsError(
            f"content manifest at {manifest_url} has no files array"
        )

    by_name: dict[str, list[dict]] = {}
    for entry in files:
        if isinstance(entry, dict) and isinstance(entry.get("name"), str):
            by_name.setdefault(entry["name"], []).append(entry)

    result = {}
    base_url = content_base_url.rstrip("/") + "/"
    for surface, logical_name in SURFACE_FILES.items():
        matches = by_name.get(logical_name, [])
        if len(matches) != 1:
            raise DefinitionsError(
                f"content manifest must contain exactly one {logical_name}; "
                f"found {len(matches)}"
            )
        digest = matches[0].get("sha256")
        if not isinstance(digest, str) or not CONTENT_HASH.fullmatch(digest):
            raise DefinitionsError(
                f"content manifest entry {logical_name} has an invalid sha256"
            )
        url = urllib.parse.urljoin(base_url, digest.lower())
        try:
            source = fetch_bytes(url).decode("utf-8")
        except UnicodeError as error:
            raise DefinitionsError(f"declarations at {url} are not UTF-8") from error
        result[surface] = (url, source, version)
    return result


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = text.rstrip("\r\n") + "\n"
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", newline="\n", dir=path.parent, delete=False
    ) as output:
        temporary = Path(output.name)
        output.write(text)
    temporary.replace(path)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Copy Luduvo Studio's installed global types into the LSP snapshots."
    )
    source = parser.add_mutually_exclusive_group()
    source.add_argument(
        "--definitions",
        type=Path,
        help="copy one declaration file; requires --surface server or client",
    )
    source.add_argument(
        "--content-root",
        type=Path,
        help="Luduvo content root containing current and versions/",
    )
    source.add_argument(
        "--data-directory",
        type=Path,
        help="Luduvo Client data directory containing content/",
    )
    source.add_argument(
        "--content-manifest-url",
        help="download declarations named by Luduvo's official content manifest",
    )
    parser.add_argument(
        "--content-base-url",
        default=CONTENT_BASE_URL,
        help="base URL for content objects referenced by --content-manifest-url",
    )
    parser.add_argument(
        "--surface",
        choices=("both", *SURFACE_FILES),
        default="both",
        help="declaration surface to copy (default: both)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="destination for a single surface; invalid with --surface both",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        if arguments.surface == "both" and arguments.definitions:
            raise DefinitionsError("--definitions requires --surface server or client")
        if arguments.surface == "both" and arguments.output:
            raise DefinitionsError("--output requires --surface server or client")

        surfaces = (
            tuple(SURFACE_FILES)
            if arguments.surface == "both"
            else (arguments.surface,)
        )
        remote = None
        if arguments.content_manifest_url:
            remote = remote_definitions(
                arguments.content_manifest_url, arguments.content_base_url
            )
            content_root = None
        elif arguments.content_root:
            content_root = arguments.content_root.expanduser().resolve()
        elif arguments.data_directory:
            content_root = arguments.data_directory.expanduser().resolve() / "content"
        else:
            content_root = default_data_directory().expanduser().resolve() / "content"

        messages = []
        for surface in surfaces:
            if arguments.definitions:
                source, text, version = explicit_definitions(arguments.definitions)
            elif remote is not None:
                source, text, version = remote[surface]
            else:
                assert content_root is not None
                source, text, version = installed_definitions(content_root, surface)

            output = (
                arguments.output.expanduser().resolve()
                if arguments.output
                else OUTPUTS[surface]
            )
            write_text(output, text)
            version_text = f" from content version {version}" if version else ""
            messages.append(
                f"Copied {surface} declarations{version_text}: {source} -> {output}"
            )
    except (DefinitionsError, UnicodeError) as error:
        print(f"error: {error}")
        return 1

    print("\n".join(messages))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
