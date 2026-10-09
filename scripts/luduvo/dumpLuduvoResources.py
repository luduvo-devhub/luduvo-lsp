"""Generate the resource-name catalog bundled by the Luduvo platform."""

from __future__ import annotations

import argparse
import json
import re
import tempfile
from pathlib import Path, PurePosixPath

from dumpLuduvoTypes import (
    CONTENT_MANIFEST_URL,
    DefinitionsError,
    current_version,
    default_data_directory,
    fetch_bytes,
)


ROOT = Path(__file__).resolve().parent / "api-docs" / "en-us"
OUTPUT = ROOT / "resources.luduvo.json"
COMPONENT_NAME = re.compile(r"[A-Za-z][A-Za-z0-9_]*\Z")


class ResourceCatalogError(RuntimeError):
    """Raised when a Luduvo resource source has an invalid shape."""


def sorted_unique(names: list[str]) -> list[str]:
    if any(not isinstance(name, str) or not name for name in names):
        raise ResourceCatalogError("resource names must be non-empty strings")
    return sorted(set(names), key=lambda name: (name.casefold(), name))


def load_component_names(
    components_path: Path = ROOT / "luduvo-components.json",
    overrides_path: Path = ROOT / "luduvo-component-overrides.json",
) -> list[str]:
    try:
        components = json.loads(components_path.read_text(encoding="utf-8"))
        overrides = json.loads(overrides_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ResourceCatalogError(f"could not read component names: {error}") from error

    if not isinstance(components, list) or not isinstance(overrides, list):
        raise ResourceCatalogError("component catalogs must be JSON arrays")
    names = sorted_unique(components + overrides)
    invalid = [name for name in names if not COMPONENT_NAME.fullmatch(name)]
    if invalid:
        raise ResourceCatalogError(f"invalid component name {invalid[0]!r}")
    return names


def prefab_names_from_manifest(manifest: object) -> tuple[str, list[str]]:
    if not isinstance(manifest, dict) or not isinstance(manifest.get("files"), list):
        raise ResourceCatalogError("content manifest has no files array")
    if "version" not in manifest:
        raise ResourceCatalogError("content manifest has no version")

    names = []
    for entry in manifest["files"]:
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
            continue
        path = PurePosixPath(entry["name"])
        if len(path.parts) == 2 and path.parts[0] == "prefabs" and path.suffix.lower() == ".ldv" and path.stem:
            names.append(path.stem)
    return str(manifest["version"]), sorted_unique(names)


def prefab_names_from_content_root(content_root: Path) -> tuple[str, list[str]]:
    version = current_version(content_root)
    directory = content_root / "versions" / version / "prefabs"
    try:
        if not directory.is_dir():
            raise ResourceCatalogError(f"prefab directory does not exist: {directory}")
        names = [path.stem for path in directory.iterdir() if path.is_file() and path.suffix.lower() == ".ldv"]
    except OSError as error:
        raise ResourceCatalogError(f"could not read prefab directory at {directory}: {error}") from error
    return version, sorted_unique(names)


def remote_prefab_names(manifest_url: str) -> tuple[str, list[str]]:
    try:
        manifest = json.loads(fetch_bytes(manifest_url))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ResourceCatalogError(f"content manifest at {manifest_url} is invalid JSON") from error
    return prefab_names_from_manifest(manifest)


def build_catalog(content_version: str, prefabs: list[str], components: list[str]) -> dict:
    return {
        "formatVersion": 1,
        "luduvoContentVersion": content_version,
        "resources": {
            "component": sorted_unique(components),
            "prefab": sorted_unique(prefabs),
        },
    }


def write_catalog(path: Path, catalog: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(catalog, indent=2, ensure_ascii=False) + "\n"
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", newline="\n", dir=path.parent, delete=False) as output:
        temporary = Path(output.name)
        output.write(text)
    temporary.replace(path)


def generate_from_content_root(content_root: Path, output: Path = OUTPUT) -> dict:
    version, prefabs = prefab_names_from_content_root(content_root)
    catalog = build_catalog(version, prefabs, load_component_names())
    write_catalog(output, catalog)
    return catalog


def generate_from_manifest(manifest_url: str, output: Path = OUTPUT) -> dict:
    version, prefabs = remote_prefab_names(manifest_url)
    catalog = build_catalog(version, prefabs, load_component_names())
    write_catalog(output, catalog)
    return catalog


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate Luduvo's bundled resource-name catalog.")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--content-root", type=Path)
    source.add_argument("--data-directory", type=Path)
    source.add_argument("--content-manifest-url")
    parser.add_argument("--output", type=Path, default=OUTPUT)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        output = arguments.output.expanduser().resolve()
        if arguments.content_manifest_url:
            catalog = generate_from_manifest(arguments.content_manifest_url, output)
        else:
            if arguments.content_root:
                content_root = arguments.content_root.expanduser().resolve()
            else:
                data_directory = (
                    arguments.data_directory.expanduser().resolve()
                    if arguments.data_directory
                    else default_data_directory().expanduser().resolve()
                )
                content_root = data_directory / "content"
            catalog = generate_from_content_root(content_root, output)
    except (DefinitionsError, ResourceCatalogError, OSError) as error:
        print(f"error: {error}")
        return 1

    resources = catalog["resources"]
    print(
        f"Generated {len(resources['component'])} component names and "
        f"{len(resources['prefab'])} prefab names from content version "
        f"{catalog['luduvoContentVersion']} in {output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
