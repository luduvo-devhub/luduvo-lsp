"""Download Luduvo's generated binding catalog and write its JSON snapshots.

The documentation site ships the same compact catalog used to build its API
reference.  Its JavaScript filename is content-hashed, so this script discovers
the current file from /reference instead of hard-coding a particular build.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import html
import json
import math
import re
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path

from dumpLuduvoTypes import (
    CONTENT_BASE_URL,
    CONTENT_MANIFEST_URL,
    OUTPUTS,
    DefinitionsError,
    default_data_directory,
    installed_definitions,
    remote_definitions,
    write_text,
)
from luduvo_declarations import merge_surfaces, parse_declarations

REFERENCE_URL = "https://docs.luduvo.com/reference"
ROOT = Path(__file__).resolve().parent
USER_AGENT = "luduvo-lsp catalog dumper"
CATALOG_MARKER = re.compile(r"version\s*:\s*\d+\s*,\s*strings\s*:\s*\[")
SCRIPT_URL = re.compile(r"(?:src|href)=[\"']([^\"']+\.js(?:\?[^\"']*)?)[\"']")
NUMBER = re.compile(r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?")


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
        raise CatalogError(
            "catalog symbols array is not a sequence of five-value records"
        )
    if len(component_data) % 3:
        raise CatalogError(
            "catalog components array is not a sequence of three-value records"
        )

    symbol_count = len(symbols) // 5
    component_count = len(component_data) // 3
    if len(symbol_components) != symbol_count:
        raise CatalogError(
            "catalog symbolComponents length does not match the number of symbols"
        )

    rows = []
    for symbol_index in range(symbol_count):
        name, type_name, doc, flags, parent = symbols[
            symbol_index * 5 : symbol_index * 5 + 5
        ]
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


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(value, indent=2, ensure_ascii=False) + "\n"
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", newline="\n", dir=path.parent, delete=False
    ) as output:
        temporary = Path(output.name)
        output.write(text)
    temporary.replace(path)


def website_link(rows: list[dict], index: int) -> str:
    row = rows[index]
    if row["parent"]:
        parent = rows[row["parent"] - 1]
        return f"{REFERENCE_URL}/{parent['name']}#{row['name']}"
    kind = row["flags"] & 7
    if kind == 6:
        return f"{REFERENCE_URL}/types/{row['name']}"
    return f"{REFERENCE_URL}/{row['name']}"


def enrich_from_website(
    api: list[dict], rows: list[dict], overrides: list[dict]
) -> None:
    """Fill declaration records with website prose and links, never website types."""
    by_key: dict[tuple[str | None, str], tuple[str, str]] = {}
    by_name: dict[str, list[tuple[str, str]]] = {}
    for index, row in enumerate(rows):
        owner = rows[row["parent"] - 1]["name"] if row["parent"] else None
        value = (row["doc"].strip(), website_link(rows, index))
        by_key[(owner, row["name"])] = value
        by_name.setdefault(row["name"], []).append(value)
    for row in overrides:
        by_key[(row.get("owner"), row["name"])] = (row.get("doc", "").strip(), "")

    for record in api:
        owner = record["owner"]
        owner_tail = owner.rsplit(".", 1)[-1] if owner else None
        value = by_key.get((owner_tail, record["name"]))
        if value is None and owner == "game":
            value = by_key.get((None, record["name"]))
        if value is None and owner is None:
            value = by_key.get((None, record["name"]))
        if value is None and len(by_name.get(record["name"], [])) == 1:
            value = by_name[record["name"]][0]
        if value is None:
            continue
        documentation, link = value
        if not record["documentation"] and documentation:
            record["documentation"] = documentation
        if link:
            record["learn_more_link"] = link


def load_definition_sources(
    arguments: argparse.Namespace,
) -> dict[str, tuple[str, str, str | None]]:
    if arguments.content_manifest_url:
        return remote_definitions(
            arguments.content_manifest_url, arguments.content_base_url
        )

    result = {}
    for surface in ("server", "client"):
        explicit_path = getattr(arguments, f"{surface}_definitions")
        if explicit_path:
            path = explicit_path.expanduser().resolve()
            result[surface] = (str(path), path.read_text(encoding="utf-8"), None)
        else:
            path, source, version = installed_definitions(
                arguments.content_root, surface
            )
            result[surface] = (str(path), source, version)
    return result


def load_local_api(sources: dict[str, tuple[str, str, str | None]]) -> list[dict]:
    parsed = {
        surface: parse_declarations(source, surface)
        for surface, (_, source, _) in sources.items()
    }
    for surface, records in parsed.items():
        if not records:
            raise CatalogError(
                f"{surface} declarations at {sources[surface][0]} contained no API records"
            )
    return merge_surfaces(parsed["server"], parsed["client"])


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Dump Luduvo's website documentation catalog as JSON."
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
        help="directory for luduvo-api.json",
    )
    parser.add_argument(
        "--data-directory",
        type=Path,
        help="Luduvo Client data directory (defaults to the platform roaming location)",
    )
    parser.add_argument(
        "--content-root",
        type=Path,
        help="Luduvo content directory containing current and versions/",
    )
    parser.add_argument(
        "--content-manifest-url",
        help=(
            "download authoritative declarations from Luduvo's content manifest; "
            f"the official URL is {CONTENT_MANIFEST_URL}"
        ),
    )
    parser.add_argument(
        "--content-base-url",
        default=CONTENT_BASE_URL,
        help="base URL for content objects referenced by --content-manifest-url",
    )
    parser.add_argument("--server-definitions", type=Path)
    parser.add_argument("--client-definitions", type=Path)
    parser.add_argument(
        "--no-docs",
        action="store_true",
        help="leave every API doc field empty",
    )
    parser.add_argument(
        "--generate",
        action="store_true",
        help="run the Luduvo type and documentation generators after updating the snapshots",
    )
    return parser.parse_args()


def run_documentation_generator() -> None:
    script = "dumpLuduvoDocs.py"
    try:
        subprocess.run([sys.executable, str(ROOT / script)], check=True)
    except subprocess.CalledProcessError as error:
        raise CatalogError(
            f"{script} failed with exit code {error.returncode}"
        ) from error


def main() -> int:
    arguments = parse_arguments()
    try:
        output_dir = arguments.output_dir.expanduser().resolve()
        if arguments.generate and output_dir != ROOT.resolve():
            raise CatalogError(
                "--generate can only be used with the default --output-dir"
            )

        if arguments.content_manifest_url and any(
            (
                arguments.content_root,
                arguments.data_directory,
                arguments.server_definitions,
                arguments.client_definitions,
            )
        ):
            raise CatalogError(
                "--content-manifest-url cannot be combined with local definition sources"
            )

        if arguments.content_manifest_url:
            arguments.content_root = None
        elif arguments.content_root:
            arguments.content_root = arguments.content_root.expanduser().resolve()
        else:
            data_directory = (
                arguments.data_directory.expanduser().resolve()
                if arguments.data_directory
                else default_data_directory().expanduser().resolve()
            )
            arguments.content_root = data_directory / "content"

        source_name, source = load_source(arguments.source, arguments.reference_url)
        version, rows = decode_catalog(source, not arguments.no_docs)
        definition_sources = load_definition_sources(arguments)
        api = load_local_api(definition_sources)
        overrides_path = ROOT / "api-docs" / "en-us" / "luduvo-api-overrides.json"
        overrides = json.loads(overrides_path.read_text(encoding="utf-8"))
        enrich_from_website(api, rows, overrides)
        write_json(output_dir / "api-docs" / "en-us" / "luduvo-api.json", api)
        if arguments.generate:
            for surface, (_, declaration_source, _) in definition_sources.items():
                write_text(OUTPUTS[surface], declaration_source)
            run_documentation_generator()
    except (CatalogError, DefinitionsError, OSError, UnicodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    print(
        f"Dumped {len(api)} local API declarations and enriched them from "
        f"website catalog version {version} at {source_name}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
