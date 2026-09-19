"""Download Luduvo's generated binding catalog and write its JSON snapshots.

The documentation site ships the same compact catalog used to build its API
reference.  Its JavaScript filename is content-hashed, so this script discovers
the current file from /reference instead of hard-coding a particular build.
"""

from __future__ import annotations

import argparse
import base64
import concurrent.futures
import hashlib
import html
import io
import json
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path


REFERENCE_URL = "https://docs.luduvo.com/reference"
STABLE_MANIFEST_URL = "https://assets.luduvo.com/channels/stable/windows-x86_64.txt"
ROOT = Path(__file__).resolve().parent
USER_AGENT = "luduvo-lsp catalog dumper"
CATALOG_MARKER = re.compile(r"version\s*:\s*\d+\s*,\s*strings\s*:\s*\[")
SCRIPT_URL = re.compile(r"(?:src|href)=[\"']([^\"']+\.js(?:\?[^\"']*)?)[\"']")
NUMBER = re.compile(r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?")
COMPONENT_NAME = re.compile(rb"[A-Za-z][A-Za-z0-9_]{1,95}\Z")
COMPONENT_ANCHOR = ("Position", "Rotation", "Scale", "Name", "Velocity")
COMPONENT_SUFFIX = ("LocalTransform", "TargetYaw", "ScriptHandles", "ScriptRef")


class CatalogError(RuntimeError):
    """Raised when the generated catalog does not have the expected shape."""


def fetch_text(url: str) -> str:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            charset = response.headers.get_content_charset() or "utf-8"
            return response.read().decode(charset)
    except OSError as error:
        raise CatalogError(f"could not download {url}: {error}") from error


def discover_catalog(reference_url: str) -> tuple[str, str]:
    page = fetch_text(reference_url)
    candidates = []
    for match in SCRIPT_URL.finditer(page):
        url = urllib.parse.urljoin(reference_url, html.unescape(match.group(1)))
        if url not in candidates:
            candidates.append(url)

    if not candidates:
        raise CatalogError(f"no JavaScript files were linked from {reference_url}")

    def download(url: str) -> tuple[str, str] | None:
        try:
            return url, fetch_text(url)
        except CatalogError:
            return None

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
        for result in executor.map(download, candidates):
            if result is None:
                continue
            url, source = result
            if CATALOG_MARKER.search(source) and all(
                re.search(rf"\b{field}\s*:\s*\[", source)
                for field in ("symbols", "components", "symbolComponents")
            ):
                return url, source

    raise CatalogError(
        "none of the JavaScript files linked from the reference page contained "
        "a Luduvo binding catalog"
    )


def load_source(source: str | None, reference_url: str) -> tuple[str, str]:
    if source is None:
        return discover_catalog(reference_url)
    if urllib.parse.urlparse(source).scheme in {"http", "https"}:
        return source, fetch_text(source)
    path = Path(source).expanduser().resolve()
    try:
        return str(path), path.read_text(encoding="utf-8")
    except OSError as error:
        raise CatalogError(f"could not read {path}: {error}") from error


def find_array(source: str, field: str, start: int) -> str:
    match = re.search(rf"\b{re.escape(field)}\s*:\s*\[", source[start:])
    if match is None:
        raise CatalogError(f"catalog has no {field!r} array")

    left = start + match.end() - 1
    depth = 0
    quote: str | None = None
    escaped = False
    for index in range(left, len(source)):
        character = source[index]
        if quote is not None:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = None
            continue
        if character in {"`", '"', "'"}:
            quote = character
        elif character == "[":
            depth += 1
        elif character == "]":
            depth -= 1
            if depth == 0:
                return source[left + 1 : index]

    raise CatalogError(f"catalog {field!r} array is not terminated")


def decode_escape(source: str, index: int) -> tuple[str, int]:
    if index >= len(source):
        raise CatalogError("unterminated escape in catalog string")
    character = source[index]
    simple = {
        "b": "\b",
        "f": "\f",
        "n": "\n",
        "r": "\r",
        "t": "\t",
        "v": "\v",
        "0": "\0",
        "\\": "\\",
        "`": "`",
        '"': '"',
        "'": "'",
    }
    if character in simple:
        return simple[character], index + 1
    if character in "\r\n":
        if character == "\r" and index + 1 < len(source) and source[index + 1] == "\n":
            index += 1
        return "", index + 1
    if character == "x":
        digits = source[index + 1 : index + 3]
        if len(digits) != 2 or not re.fullmatch(r"[0-9a-fA-F]{2}", digits):
            raise CatalogError("invalid hexadecimal escape in catalog string")
        return chr(int(digits, 16)), index + 3
    if character == "u":
        if index + 1 < len(source) and source[index + 1] == "{":
            right = source.find("}", index + 2)
            if right < 0:
                raise CatalogError("unterminated Unicode escape in catalog string")
            digits = source[index + 2 : right]
            if not re.fullmatch(r"[0-9a-fA-F]{1,6}", digits):
                raise CatalogError("invalid Unicode escape in catalog string")
            return chr(int(digits, 16)), right + 1
        digits = source[index + 1 : index + 5]
        if len(digits) != 4 or not re.fullmatch(r"[0-9a-fA-F]{4}", digits):
            raise CatalogError("invalid Unicode escape in catalog string")
        return chr(int(digits, 16)), index + 5
    # JavaScript treats an otherwise unknown escape as the escaped character.
    return character, index + 1


def parse_strings(source: str) -> list[str]:
    values = []
    index = 0
    while index < len(source):
        while index < len(source) and (source[index].isspace() or source[index] == ","):
            index += 1
        if index == len(source):
            break
        quote = source[index]
        if quote not in {"`", '"', "'"}:
            raise CatalogError(f"expected a catalog string at offset {index}")
        index += 1
        value = []
        while index < len(source):
            character = source[index]
            if character == quote:
                index += 1
                break
            if character == "\\":
                decoded, index = decode_escape(source, index + 1)
                value.append(decoded)
                continue
            if quote == "`" and source.startswith("${", index):
                raise CatalogError("catalog contains an interpolated template string")
            value.append(character)
            index += 1
        else:
            raise CatalogError("unterminated string in catalog string table")
        values.append("".join(value))
    return values


def parse_integers(source: str, field: str) -> list[int]:
    values = []
    position = 0
    for match in NUMBER.finditer(source):
        separator = source[position : match.start()]
        if separator.strip(" \t\r\n,"):
            raise CatalogError(f"unexpected value in catalog {field!r} array")
        number = float(match.group())
        if not math.isfinite(number) or not number.is_integer():
            raise CatalogError(f"non-integer value in catalog {field!r} array")
        values.append(int(number))
        position = match.end()
    if source[position:].strip(" \t\r\n,"):
        raise CatalogError(f"unexpected trailing value in catalog {field!r} array")
    return values


def string_at(strings: list[str], index: int, field: str) -> str:
    try:
        return strings[index]
    except IndexError as error:
        raise CatalogError(f"{field} refers to missing string {index}") from error


def decode_catalog(source: str, include_docs: bool) -> tuple[int, list[dict]]:
    marker = CATALOG_MARKER.search(source)
    if marker is None:
        raise CatalogError("source does not contain a Luduvo binding catalog")
    catalog_start = marker.start()
    version_match = re.search(r"version\s*:\s*(\d+)", source[catalog_start:])
    if version_match is None:
        raise CatalogError("catalog has no numeric version")
    version = int(version_match.group(1))

    strings = parse_strings(find_array(source, "strings", catalog_start))
    symbols = parse_integers(find_array(source, "symbols", catalog_start), "symbols")
    component_data = parse_integers(
        find_array(source, "components", catalog_start), "components"
    )
    symbol_components = parse_integers(
        find_array(source, "symbolComponents", catalog_start), "symbolComponents"
    )

    if len(symbols) % 5:
        raise CatalogError("catalog symbols array is not a sequence of five-value records")
    if len(component_data) % 3:
        raise CatalogError("catalog components array is not a sequence of three-value records")

    symbol_count = len(symbols) // 5
    component_count = len(component_data) // 3
    if len(symbol_components) != symbol_count:
        raise CatalogError(
            "catalog symbolComponents length does not match the number of symbols"
        )

    rows = []
    for symbol_index in range(symbol_count):
        name, type_name, doc, flags, parent = symbols[symbol_index * 5 : symbol_index * 5 + 5]
        component = symbol_components[symbol_index]
        if parent < 0 or parent > symbol_count:
            raise CatalogError(f"symbol {symbol_index + 1} has invalid parent {parent}")
        if component < 0 or component > component_count:
            raise CatalogError(
                f"symbol {symbol_index + 1} has invalid component {component}"
            )
        rows.append(
            {
                "name": string_at(strings, name, "symbol name"),
                "type": string_at(strings, type_name, "symbol type"),
                "doc": string_at(strings, doc, "symbol doc") if include_docs else "",
                "flags": flags,
                "parent": parent,
                "component": component,
            }
        )

    return version, rows


def pe_layout(executable: bytes) -> tuple[int, list[tuple[int, int, int, int]]]:
    """Return the image base and (RVA, virtual size, raw offset, raw size) sections."""
    try:
        pe_offset = struct.unpack_from("<I", executable, 0x3C)[0]
        if executable[pe_offset : pe_offset + 4] != b"PE\0\0":
            raise CatalogError("component source is not a PE executable")
        section_count = struct.unpack_from("<H", executable, pe_offset + 6)[0]
        optional_size = struct.unpack_from("<H", executable, pe_offset + 20)[0]
        optional_offset = pe_offset + 24
        magic = struct.unpack_from("<H", executable, optional_offset)[0]
        if magic == 0x20B:
            image_base = struct.unpack_from("<Q", executable, optional_offset + 24)[0]
        elif magic == 0x10B:
            image_base = struct.unpack_from("<I", executable, optional_offset + 28)[0]
        else:
            raise CatalogError(f"unsupported PE optional-header magic 0x{magic:x}")
        section_offset = optional_offset + optional_size
        sections = []
        for index in range(section_count):
            offset = section_offset + index * 40
            virtual_size, rva, raw_size, raw_offset = struct.unpack_from(
                "<IIII", executable, offset + 8
            )
            sections.append((rva, virtual_size, raw_offset, raw_size))
    except (IndexError, struct.error) as error:
        raise CatalogError("component source has a truncated PE header") from error
    return image_base, sections


def component_names_from_executable(executable: bytes) -> list[str]:
    image_base, sections = pe_layout(executable)

    def offset_pointer(offset: int) -> int | None:
        for section_rva, _virtual_size, raw_offset, raw_size in sections:
            if raw_offset <= offset < raw_offset + raw_size:
                return image_base + section_rva + offset - raw_offset
        return None

    def pointer_string(pointer: int) -> bytes | None:
        rva = pointer - image_base
        for section_rva, virtual_size, raw_offset, raw_size in sections:
            span = max(virtual_size, raw_size)
            if section_rva <= rva < section_rva + span:
                offset = raw_offset + rva - section_rva
                if offset < 0 or offset >= len(executable):
                    return None
                end = executable.find(b"\0", offset, min(offset + 97, len(executable)))
                if end < 0:
                    return None
                value = executable[offset:end]
                return value if COMPONENT_NAME.fullmatch(value) else None
        return None

    pointer_size = 8 if image_base > 0xFFFFFFFF else 4
    pointer_format = "<Q" if pointer_size == 8 else "<I"
    anchor = tuple(name.encode("ascii") for name in COMPONENT_ANCHOR)
    candidates = []
    position = 0
    while True:
        position = executable.find(anchor[0] + b"\0", position)
        if position < 0:
            break
        pointer = offset_pointer(position)
        position += 1
        if pointer is None:
            continue
        packed_pointer = struct.pack(pointer_format, pointer)
        pointer_offset = 0
        while True:
            pointer_offset = executable.find(packed_pointer, pointer_offset)
            if pointer_offset < 0:
                break
            if pointer_offset % pointer_size == 0:
                names = []
                for index in range(len(anchor)):
                    item = struct.unpack_from(
                        pointer_format, executable, pointer_offset + index * pointer_size
                    )[0]
                    names.append(pointer_string(item))
                if tuple(names) == anchor:
                    candidates.append(pointer_offset)
            pointer_offset += 1

    if len(candidates) != 1:
        raise CatalogError(
            "expected exactly one Position/Rotation/Scale/Name/Velocity component "
            f"registry in LuduvoGame.exe, found {len(candidates)}"
        )

    components = []
    offset = candidates[0]
    while offset + pointer_size <= len(executable):
        pointer = struct.unpack_from(pointer_format, executable, offset)[0]
        value = pointer_string(pointer)
        if value is None:
            break
        components.append(value.decode("ascii"))
        offset += pointer_size

    if len(components) < len(anchor) or tuple(components[: len(anchor)]) != COMPONENT_ANCHOR:
        raise CatalogError("component registry failed its prefix validation")
    if tuple(components[-len(COMPONENT_SUFFIX) :]) != COMPONENT_SUFFIX:
        raise CatalogError("component registry failed its suffix validation")
    if len(set(components)) != len(components):
        raise CatalogError("component registry contains duplicate identifiers")
    return sorted(components, key=str.casefold)


def default_bundle_dir() -> Path:
    local_app_data = os.environ.get("LOCALAPPDATA")
    if local_app_data:
        return Path(local_app_data) / "Luduvo" / "bundle"
    return Path.home() / "AppData" / "Local" / "Luduvo" / "bundle"


def download_stable_game() -> tuple[str, bytes]:
    manifest_text = fetch_text(STABLE_MANIFEST_URL)
    lines = manifest_text.splitlines()
    if len(lines) != 3 or lines[0] != "LDV1":
        raise CatalogError("stable release manifest has an unrecognized format")
    try:
        payload = json.loads(base64.b64decode(lines[2], validate=True))
        platform = payload["platforms"]["windows-x86_64"]
        bundle = platform["bundle"]
        game_name = platform["game_exe"]
        bundle_url = bundle["url"]
        expected_size = int(bundle["size"])
        expected_hash = bundle["sha256"].lower()
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise CatalogError("stable release manifest is missing bundle metadata") from error

    if expected_size <= 0 or expected_size > 2_000_000_000:
        raise CatalogError(f"stable bundle has unreasonable size {expected_size}")
    print(
        f"LuduvoGame.exe was not found locally; downloading stable release "
        f"{payload.get('version', '?')} ({expected_size / 1_000_000:.1f} MB)..."
    )
    request = urllib.request.Request(bundle_url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            archive = response.read(expected_size + 1)
    except OSError as error:
        raise CatalogError(f"could not download {bundle_url}: {error}") from error
    if len(archive) != expected_size:
        raise CatalogError(
            f"stable bundle size mismatch: expected {expected_size}, got {len(archive)}"
        )
    actual_hash = hashlib.sha256(archive).hexdigest()
    if actual_hash != expected_hash:
        raise CatalogError(
            f"stable bundle SHA-256 mismatch: expected {expected_hash}, got {actual_hash}"
        )

    try:
        with zipfile.ZipFile(io.BytesIO(archive)) as bundle_zip:
            matches = [
                name
                for name in bundle_zip.namelist()
                if Path(name).name.casefold() == game_name.casefold()
            ]
            if len(matches) != 1:
                raise CatalogError(
                    f"stable bundle contains {len(matches)} files named {game_name}"
                )
            return f"{bundle_url}!/{matches[0]}", bundle_zip.read(matches[0])
    except zipfile.BadZipFile as error:
        raise CatalogError("stable bundle is not a valid ZIP archive") from error


def load_game_executable(
    game_exe: Path | None, bundle_dir: Path, allow_download: bool
) -> tuple[str, bytes]:
    path = game_exe.expanduser().resolve() if game_exe else bundle_dir / "LuduvoGame.exe"
    if path.is_file():
        try:
            return str(path), path.read_bytes()
        except OSError as error:
            raise CatalogError(f"could not read {path}: {error}") from error
    if allow_download and game_exe is None:
        return download_stable_game()
    raise CatalogError(
        f"could not find {path}; pass --game-exe or allow the stable release download"
    )


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(value, indent=2, ensure_ascii=False) + "\n"
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", newline="\n", dir=path.parent, delete=False
    ) as output:
        temporary = Path(output.name)
        output.write(text)
    temporary.replace(path)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Dump Luduvo's generated API and component catalogs as JSON."
    )
    parser.add_argument(
        "--reference-url",
        default=REFERENCE_URL,
        help="reference index used to discover the current catalog chunk",
    )
    parser.add_argument(
        "--source",
        help="read a known catalog JavaScript file or URL instead of discovering it",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT,
        help="directory for luduvo-api.json and luduvo-components.json",
    )
    parser.add_argument(
        "--no-docs",
        action="store_true",
        help="leave every API doc field empty",
    )
    parser.add_argument(
        "--bundle-dir",
        type=Path,
        default=default_bundle_dir(),
        help="installed Luduvo bundle directory containing LuduvoGame.exe",
    )
    parser.add_argument(
        "--game-exe",
        type=Path,
        help="read component identifiers from this LuduvoGame.exe",
    )
    parser.add_argument(
        "--no-download",
        action="store_true",
        help="fail instead of downloading the current stable bundle when the game executable is absent",
    )
    parser.add_argument(
        "--generate",
        action="store_true",
        help="run the Luduvo type and documentation generators after updating the snapshots",
    )
    return parser.parse_args()


def run_generators() -> None:
    for script in ("dumpLuduvoTypes.py", "dumpLuduvoDocs.py"):
        try:
            subprocess.run([sys.executable, str(ROOT / script)], check=True)
        except subprocess.CalledProcessError as error:
            raise CatalogError(f"{script} failed with exit code {error.returncode}") from error


def main() -> int:
    arguments = parse_arguments()
    try:
        output_dir = arguments.output_dir.expanduser().resolve()
        if arguments.generate and output_dir != ROOT.resolve():
            raise CatalogError("--generate can only be used with the default --output-dir")

        source_name, source = load_source(arguments.source, arguments.reference_url)
        version, rows = decode_catalog(source, not arguments.no_docs)
        game_source, executable = load_game_executable(
            arguments.game_exe,
            arguments.bundle_dir.expanduser().resolve(),
            not arguments.no_download,
        )
        components = component_names_from_executable(executable)
        write_json(output_dir / "api-docs" / "en-us" / "luduvo-api.json", rows)
        write_json(output_dir / "api-docs" / "en-us" / "luduvo-components.json", components)
        if arguments.generate:
            run_generators()
    except CatalogError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    print(
        f"Dumped catalog version {version}: {len(rows)} API symbols and "
        f"{len(components)} components from {game_source}; API source: {source_name}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
