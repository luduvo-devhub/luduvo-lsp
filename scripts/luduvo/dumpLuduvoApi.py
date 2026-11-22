"""Refresh Luduvo API declarations and enrich them from the documentation index."""

from __future__ import annotations

import argparse
import json
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
from dumpLuduvoResources import OUTPUT as RESOURCES_OUTPUT, ResourceCatalogError, generate_from_content_root, generate_from_manifest, write_catalog
from dumpLuduvoDeclarationDocs import merge_surfaces, parse_declarations
from dumpLuduvoDocs import WebsiteIndexError, enrich_from_website, object_resources

REFERENCE_URL = "https://docs.luduvo.com/reference/"
ROOT = Path(__file__).resolve().parent
USER_AGENT = "luduvo-lsp catalog dumper"


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


def load_source(source: str | None, reference_url: str) -> tuple[str, str]:
    if source is None:
        source = urllib.parse.urljoin(reference_url.rstrip("/") + "/", "../search.json")
    if urllib.parse.urlparse(source).scheme in {"http", "https"}:
        return source, fetch_text(source)
    path = Path(source).expanduser().resolve()
    return str(path), path.read_text(encoding="utf-8")


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(value, indent=2, ensure_ascii=False) + "\n"
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", newline="\n", dir=path.parent, delete=False
    ) as output:
        temporary = Path(output.name)
        output.write(text)
    temporary.replace(path)


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
        description="Dump Luduvo's declarations and website documentation as JSON."
    )
    parser.add_argument(
        "--reference-url",
        default=REFERENCE_URL,
        help="reference URL used to locate the search index and build documentation links",
    )
    parser.add_argument(
        "--source",
        help="read a Zensical search index JSON file or URL",
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
        help="skip website documentation and keep declaration comments",
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

        definition_sources = load_definition_sources(arguments)
        api = load_local_api(definition_sources)
        overrides_path = ROOT / "api-docs" / "en-us" / "luduvo-api-overrides.json"
        overrides = json.loads(overrides_path.read_text(encoding="utf-8"))
        by_override = {(row.get("owner"), row["name"]): row["doc"].strip() for row in overrides}
        for record in api:
            key = (record["owner"], record["name"])
            if key in by_override and not record["documentation"]:
                record["documentation"] = by_override[key]
        source_name = None
        if not arguments.no_docs:
            source_name, source = load_source(arguments.source, arguments.reference_url)
            enrich_from_website(api, source, arguments.reference_url)
        write_json(output_dir / "api-docs" / "en-us" / "luduvo-api.json", api)
        if arguments.generate:
            objects = object_resources(source, arguments.reference_url, fetch_text) if source_name else None
            for surface, (_, declaration_source, _) in definition_sources.items():
                write_text(OUTPUTS[surface], declaration_source)
            if arguments.content_manifest_url:
                resources = generate_from_manifest(arguments.content_manifest_url)
            else:
                resources = generate_from_content_root(arguments.content_root)
            if objects:
                resources["resources"]["object"] = objects
                write_catalog(RESOURCES_OUTPUT, resources)
            run_documentation_generator()
    except (CatalogError, DefinitionsError, ResourceCatalogError, WebsiteIndexError, OSError, UnicodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    message = f"Dumped {len(api)} API declarations"
    if source_name:
        message += f" and enriched them from {source_name}"
    print(message)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
