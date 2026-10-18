"""Generate the resource-name catalog bundled by the Luduvo platform."""

from __future__ import annotations

import argparse
import base64
import io
import json
import os
import platform
import re
import struct
import tempfile
import zipfile
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
INSTANCE_DECLARATION = re.compile(r"^\s*declare\s+extern\s+type\s+Instance\s+with\s*$")
INSTANCE_COMPONENT = re.compile(
    r"^\s*(?:read\s+)?([A-Za-z][A-Za-z0-9_]*)\s*:\s*[A-Za-z][A-Za-z0-9_]*Component\??\s*,?\s*$"
)
DEFINITION_PATHS = (
    ROOT / "globalTypes.luduvo.server.d.luau",
    ROOT / "globalTypes.luduvo.client.d.luau",
)
RELEASE_CHANNEL_URL = "https://assets.luduvo.com/channels/stable/windows-x86_64.txt"


class ResourceCatalogError(RuntimeError):
    """Raised when a Luduvo resource source has an invalid shape."""


class PortableExecutable:
    """Read the small portion of PE metadata needed by the component extractor."""

    def __init__(self, data: bytes):
        self.data = data
        try:
            if data[:2] != b"MZ":
                raise ResourceCatalogError("LuduvoEditor is not a PE executable")
            pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
            if data[pe_offset : pe_offset + 4] != b"PE\0\0":
                raise ResourceCatalogError("LuduvoEditor has no PE header")
            section_count = struct.unpack_from("<H", data, pe_offset + 6)[0]
            optional_size = struct.unpack_from("<H", data, pe_offset + 20)[0]
            optional_offset = pe_offset + 24
            if struct.unpack_from("<H", data, optional_offset)[0] != 0x20B:
                raise ResourceCatalogError("LuduvoEditor is not a 64-bit PE executable")
            self.image_base = struct.unpack_from("<Q", data, optional_offset + 24)[0]
            section_offset = optional_offset + optional_size
            self.sections: list[tuple[str, int, int, int, int]] = []
            for index in range(section_count):
                offset = section_offset + index * 40
                name = data[offset : offset + 8].split(b"\0", 1)[0].decode("ascii")
                virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", data, offset + 8)
                self.sections.append((name, virtual_address, max(virtual_size, raw_size), raw_offset, raw_size))
        except (IndexError, UnicodeDecodeError, struct.error) as error:
            raise ResourceCatalogError(f"LuduvoEditor has malformed PE metadata: {error}") from error

    def section(self, name: str) -> tuple[int, int]:
        for section_name, _, _, raw_offset, raw_size in self.sections:
            if section_name == name:
                return raw_offset, raw_size
        raise ResourceCatalogError(f"LuduvoEditor has no {name} section")

    def offset_to_address(self, offset: int) -> int | None:
        for _, virtual_address, _, raw_offset, raw_size in self.sections:
            if raw_offset <= offset < raw_offset + raw_size:
                return self.image_base + virtual_address + offset - raw_offset
        return None

    def address_to_offset(self, address: int) -> int | None:
        relative = address - self.image_base
        for _, virtual_address, _, raw_offset, raw_size in self.sections:
            if virtual_address <= relative < virtual_address + raw_size:
                offset = raw_offset + relative - virtual_address
                return offset if 0 <= offset < len(self.data) else None
        return None

    def contains_address(self, address: int) -> bool:
        return self.address_to_offset(address) is not None

    def string(self, address: int, limit: int = 4096) -> str | None:
        offset = self.address_to_offset(address)
        if offset is None:
            return None
        raw = self.data[offset : offset + limit].split(b"\0", 1)[0]
        try:
            value = raw.decode("utf-8")
        except UnicodeDecodeError:
            return None
        if not value or any(not (character.isprintable() or character in "\n\r\t") for character in value):
            return None
        return value


def extract_component_descriptions(editor: bytes, component_names: list[str]) -> dict[str, str]:
    image = PortableExecutable(editor)
    rdata_offset, rdata_size = image.section(".rdata")
    name_addresses: dict[int, str] = {}
    for name in component_names:
        needle = name.encode("utf-8") + b"\0"
        position = 0
        while (position := editor.find(needle, position)) >= 0:
            address = image.offset_to_address(position)
            if address is not None:
                name_addresses[address] = name
            position += 1

    candidates: dict[str, list[str]] = {}
    scan_end = min(rdata_offset + rdata_size, len(editor)) - 56
    for offset in range(rdata_offset, scan_end + 1, 8):
        values = struct.unpack_from("<7Q", editor, offset)
        name = name_addresses.get(values[0])
        if name is None:
            continue
        category = image.string(values[1], 64)
        metadata = values[2]
        field_count = values[3]
        description_address = values[4]
        flags = values[5]
        reserved = values[6]
        description = "" if description_address == 0 else image.string(description_address)
        if (
            category is None
            or len(category) > 40
            or (metadata != 0 and not image.contains_address(metadata))
            or field_count > 256
            or description is None
            or flags > 0xFFFF
            or reserved != 0
        ):
            continue
        candidates.setdefault(name, []).append(description)

    ambiguous = sorted(name for name, matches in candidates.items() if len(matches) != 1)
    if ambiguous:
        raise ResourceCatalogError(f"ambiguous component metadata for {ambiguous[0]!r}")
    minimum_recognized = max(1, len(component_names) // 2)
    if len(candidates) < minimum_recognized:
        raise ResourceCatalogError(
            f"LuduvoEditor component metadata layout changed: recognized {len(candidates)} of {len(component_names)} components"
        )
    return {name: matches[0] for name, matches in candidates.items() if matches[0]}


def release_manifest(channel_url: str = RELEASE_CHANNEL_URL) -> dict:
    lines = fetch_bytes(channel_url).decode("utf-8").splitlines()
    if len(lines) != 3 or lines[0] != "LDV1":
        raise ResourceCatalogError(f"release channel at {channel_url} has an unsupported format")
    try:
        manifest = json.loads(base64.b64decode(lines[2], validate=True))
    except (ValueError, UnicodeError, json.JSONDecodeError) as error:
        raise ResourceCatalogError(f"release channel at {channel_url} has an invalid manifest: {error}") from error
    if not isinstance(manifest, dict):
        raise ResourceCatalogError(f"release channel at {channel_url} has no manifest object")
    return manifest


def remote_editor_binary(channel_url: str = RELEASE_CHANNEL_URL) -> bytes:
    manifest = release_manifest(channel_url)
    try:
        bundle_url = manifest["platforms"]["windows-x86_64"]["bundle"]["url"]
    except (KeyError, TypeError) as error:
        raise ResourceCatalogError("release manifest has no Windows bundle URL") from error
    if not isinstance(bundle_url, str) or not bundle_url.startswith("https://"):
        raise ResourceCatalogError("release manifest has an invalid Windows bundle URL")
    try:
        with zipfile.ZipFile(io.BytesIO(fetch_bytes(bundle_url))) as bundle:
            entries = [name for name in bundle.namelist() if PurePosixPath(name.replace("\\", "/")).name == "LuduvoEditor.exe"]
            if len(entries) != 1:
                raise ResourceCatalogError(f"Windows bundle contains {len(entries)} LuduvoEditor.exe entries")
            return bundle.read(entries[0])
    except zipfile.BadZipFile as error:
        raise ResourceCatalogError(f"Windows bundle at {bundle_url} is not a ZIP archive") from error


def default_editor_binary() -> Path | None:
    if platform.system() == "Windows" and (local_app_data := os.environ.get("LOCALAPPDATA")):
        return Path(local_app_data) / "Luduvo" / "bundle" / "LuduvoEditor.exe"
    return None


def sorted_unique(names: list[str]) -> list[str]:
    if any(not isinstance(name, str) or not name for name in names):
        raise ResourceCatalogError("resource names must be non-empty strings")
    return sorted(set(names), key=lambda name: (name.casefold(), name))


def instance_component_names(declaration_sources: list[str]) -> list[str]:
    names = []
    for source in declaration_sources:
        inside_instance = False
        for line in source.splitlines():
            if not inside_instance:
                inside_instance = INSTANCE_DECLARATION.fullmatch(line) is not None
                continue
            if line.strip() == "end":
                inside_instance = False
                continue
            if match := INSTANCE_COMPONENT.fullmatch(line):
                names.append(match.group(1))
    return sorted_unique(names)


def load_component_names(
    components_path: Path = ROOT / "luduvo-components.json",
    overrides_path: Path = ROOT / "luduvo-component-overrides.json",
    definition_paths: tuple[Path, ...] | None = None,
) -> list[str]:
    try:
        components = json.loads(components_path.read_text(encoding="utf-8"))
        overrides = json.loads(overrides_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ResourceCatalogError(f"could not read component names: {error}") from error

    if not isinstance(components, list) or not isinstance(overrides, list):
        raise ResourceCatalogError("component catalogs must be JSON arrays")
    paths = DEFINITION_PATHS if definition_paths is None else definition_paths
    try:
        declaration_sources = [path.read_text(encoding="utf-8") for path in paths]
    except OSError as error:
        raise ResourceCatalogError(f"could not read Luduvo declarations for component names: {error}") from error

    names = sorted_unique(components + overrides + instance_component_names(declaration_sources))
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


def build_catalog(
    content_version: str,
    prefabs: list[str],
    components: list[str],
    component_descriptions: dict[str, str] | None = None,
) -> dict:
    descriptions = component_descriptions or {}
    component_entries = []
    for name in sorted_unique(components):
        entry = {"name": name}
        if documentation := descriptions.get(name):
            entry["documentation"] = documentation
        component_entries.append(entry)
    return {
        "formatVersion": 2,
        "luduvoContentVersion": content_version,
        "resources": {
            "component": component_entries,
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


def generate_from_content_root(
    content_root: Path,
    output: Path = OUTPUT,
    editor_binary: Path | None = None,
) -> dict:
    version, prefabs = prefab_names_from_content_root(content_root)
    components = load_component_names()
    editor_path = editor_binary or default_editor_binary()
    if editor_path is None:
        editor = remote_editor_binary()
    else:
        if not editor_path.is_file():
            raise ResourceCatalogError(f"could not find LuduvoEditor at {editor_path}")
        try:
            editor = editor_path.read_bytes()
        except OSError as error:
            raise ResourceCatalogError(f"could not read LuduvoEditor at {editor_path}: {error}") from error
    catalog = build_catalog(version, prefabs, components, extract_component_descriptions(editor, components))
    write_catalog(output, catalog)
    return catalog


def generate_from_manifest(
    manifest_url: str,
    output: Path = OUTPUT,
    release_channel_url: str = RELEASE_CHANNEL_URL,
) -> dict:
    version, prefabs = remote_prefab_names(manifest_url)
    components = load_component_names()
    descriptions = extract_component_descriptions(remote_editor_binary(release_channel_url), components)
    catalog = build_catalog(version, prefabs, components, descriptions)
    write_catalog(output, catalog)
    return catalog


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate Luduvo's bundled resource-name catalog.")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--content-root", type=Path)
    source.add_argument("--data-directory", type=Path)
    source.add_argument("--content-manifest-url")
    parser.add_argument("--editor-binary", type=Path)
    parser.add_argument("--release-channel-url", default=RELEASE_CHANNEL_URL)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        output = arguments.output.expanduser().resolve()
        if arguments.content_manifest_url:
            catalog = generate_from_manifest(arguments.content_manifest_url, output, arguments.release_channel_url)
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
            editor_binary = arguments.editor_binary.expanduser().resolve() if arguments.editor_binary else None
            catalog = generate_from_content_root(content_root, output, editor_binary)
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
