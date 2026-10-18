"""Just for fun"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


DEFAULT_API_PATH = Path(__file__).parent / "api-docs" / "en-us" / "luduvo-api.json"
DEFAULT_RESOURCES_PATH = Path(__file__).parent / "api-docs" / "en-us" / "resources.luduvo.json"
INSTANCE_METHOD = re.compile(r"^\(\s*self\s*:\s*Instance\b")
MEMBER_KINDS = {"type_member", "global_member"}
README_STATS_START = "<!-- luduvo-api-stats:start -->"
README_STATS_END = "<!-- luduvo-api-stats:end -->"


def load_entries(path: Path) -> list[dict[str, Any]]:
    with path.open(encoding="utf-8") as api_file:
        entries = json.load(api_file)

    if not isinstance(entries, list) or not all(isinstance(entry, dict) for entry in entries):
        raise ValueError(f"{path} must contain a JSON array of objects")

    return entries


def count_entries(entries: list[dict[str, Any]]) -> dict[str, int]:
    documented = lambda entry: bool(str(entry.get("documentation", "")).strip())
    type_declarations = [entry for entry in entries if entry.get("kind") == "type"]
    global_declarations = [entry for entry in entries if entry.get("kind") == "global"]
    members = [entry for entry in entries if entry.get("kind") in MEMBER_KINDS]
    instance_members = [entry for entry in members if entry.get("owner") == "Instance"]
    instance_methods = [
        entry
        for entry in instance_members
        if entry.get("kind") == "type_member"
        and bool(INSTANCE_METHOD.match(str(entry.get("type", ""))))
    ]
    instance_static_members = [entry for entry in instance_members if entry.get("kind") == "global_member"]
    instance_properties = [
        entry for entry in instance_members if entry not in instance_methods and entry not in instance_static_members
    ]

    direct_game_members = [entry for entry in members if entry.get("owner") == "game"]
    game_services = [
        entry
        for entry in direct_game_members
        if entry.get("type") == "table" or entry.get("name") == "World"
    ]
    game_service_owners = {
        "World" if entry.get("name") == "World" else f"game.{entry['name']}"
        for entry in game_services
    }
    game_service_members = [entry for entry in members if entry.get("owner") in game_service_owners]
    table_game_services = [entry for entry in game_services if entry.get("name") != "World"]
    table_game_service_owners = {f"game.{entry['name']}" for entry in table_game_services}
    table_game_service_members = [entry for entry in members if entry.get("owner") in table_game_service_owners]
    other_game_symbols = [entry for entry in direct_game_members if entry not in game_services]

    other_type_declarations = [entry for entry in type_declarations if entry.get("name") not in {"Instance", "World"}]
    other_global_declarations = [entry for entry in global_declarations if entry.get("name") not in {"game", "Instance"}]
    other_type_members = [
        entry for entry in members if entry.get("kind") == "type_member" and entry.get("owner") not in {"Instance", "World"}
    ]
    other_global_members = [
        entry
        for entry in members
        if entry.get("kind") == "global_member"
        and entry.get("owner") != "Instance"
        and entry.get("owner") != "game"
        and entry.get("owner") not in game_service_owners
    ]
    type_member_owners = {entry.get("owner") for entry in other_type_members}
    global_member_owners = {entry.get("owner") for entry in other_global_members}
    types_with_members = [entry for entry in other_type_declarations if entry.get("name") in type_member_owners]
    types_without_members = [entry for entry in other_type_declarations if entry.get("name") not in type_member_owners]
    globals_with_members = [entry for entry in other_global_declarations if entry.get("name") in global_member_owners]
    globals_without_members = [entry for entry in other_global_declarations if entry.get("name") not in global_member_owners]
    all_type_member_owners = {entry.get("owner") for entry in members if entry.get("kind") == "type_member"}
    all_global_member_owners = {entry.get("owner") for entry in members if entry.get("kind") == "global_member"}
    all_types_with_members = [entry for entry in type_declarations if entry.get("name") in all_type_member_owners]
    all_globals_with_members = [entry for entry in global_declarations if entry.get("name") in all_global_member_owners]
    instance_type_members = [entry for entry in instance_members if entry.get("kind") == "type_member"]
    world_type_members = [entry for entry in members if entry.get("kind") == "type_member" and entry.get("owner") == "World"]

    instance_declarations = [
        entry
        for entry in entries
        if (entry.get("kind"), entry.get("name")) in {("type", "Instance"), ("global", "Instance")}
    ]
    game_declarations = [
        entry
        for entry in entries
        if (entry.get("kind"), entry.get("name")) in {("type", "World"), ("global", "game")}
    ]

    def add_counts(result: dict[str, int], label: str, values: list[dict[str, Any]]) -> None:
        result[label] = len(values)
        result[f"{label} with docs"] = sum(documented(entry) for entry in values)

    result = {
        "Total entries": len(entries),
        "Entries with docs": sum(documented(entry) for entry in entries),
        "Members": len(members),
    }
    for label, values in (
        ("Type declarations", type_declarations),
        ("Global declarations", global_declarations),
        ("Type members", [entry for entry in members if entry.get("kind") == "type_member"]),
        ("Global members", [entry for entry in members if entry.get("kind") == "global_member"]),
        ("All types with members", all_types_with_members),
        ("All globals with members", all_globals_with_members),
        ("Other type declarations", other_type_declarations),
        ("Types with members", types_with_members),
        ("Types without members", types_without_members),
        ("Other type members", other_type_members),
        ("Other global declarations", other_global_declarations),
        ("Globals with members", globals_with_members),
        ("Globals without members", globals_without_members),
        ("Other global members", other_global_members),
        ("Instance declarations", instance_declarations),
        ("Instance members", instance_members),
        ("Instance type members", instance_type_members),
        ("Instance methods", instance_methods),
        ("Instance static members", instance_static_members),
        ("Instance properties", instance_properties),
        ("Game declarations", game_declarations),
        ("Direct game members", direct_game_members),
        ("Game services", game_services),
        ("Game service members", game_service_members),
        ("Table game services", table_game_services),
        ("Table game service members", table_game_service_members),
        ("World type members", world_type_members),
        ("Other game symbols", other_game_symbols),
    ):
        add_counts(result, label, values)

    result["Type entries"] = result["Type declarations"] + result["Type members"]
    result["Type entries with docs"] = result["Type declarations with docs"] + result["Type members with docs"]
    result["Global entries"] = result["Global declarations"] + result["Global members"]
    result["Global entries with docs"] = (
        result["Global declarations with docs"] + result["Global members with docs"]
    )
    result["Other declaration entries"] = (
        result["Other type declarations"]
        + result["Other type members"]
        + result["Other global declarations"]
        + result["Other global members"]
    )
    result["Other declaration entries with docs"] = (
        result["Other type declarations with docs"]
        + result["Other type members with docs"]
        + result["Other global declarations with docs"]
        + result["Other global members with docs"]
    )
    result["Instance entries"] = result["Instance declarations"] + result["Instance members"]
    result["Instance entries with docs"] = result["Instance declarations with docs"] + result["Instance members with docs"]
    result["Game members"] = result["Direct game members"] + result["Game service members"]
    result["Game members with docs"] = result["Direct game members with docs"] + result["Game service members with docs"]
    result["Game entries"] = result["Game declarations"] + result["Game members"]
    result["Game entries with docs"] = result["Game declarations with docs"] + result["Game members with docs"]
    return result


def count_resources(path: Path) -> dict[str, int]:
    with path.open(encoding="utf-8") as resources_file:
        catalog = json.load(resources_file)

    if not isinstance(catalog, dict):
        raise ValueError(f"{path} must contain a JSON object")
    resources = catalog.get("resources")
    if not isinstance(resources, dict):
        raise ValueError(f"{path} must contain a resources object")

    counts: dict[str, int] = {}
    for kind, label in (("component", "Components"), ("prefab", "Prefabs")):
        entries = resources.get(kind)
        if not isinstance(entries, list):
            raise ValueError(f"{path} must contain a resources.{kind} array")
        counts[label] = len(entries)
        counts[f"{label} with docs"] = sum(
            isinstance(entry, dict)
            and bool(str(entry.get("documentation", "")).strip())
            for entry in entries
        )
    return counts


def collect_counts(api_path: Path, resources_path: Path) -> dict[str, int]:
    counts = count_entries(load_entries(api_path))
    counts.update(count_resources(resources_path))
    counts["Catalog entries"] = counts["Total entries"] + counts["Components"] + counts["Prefabs"]
    counts["Catalog entries with docs"] = (
        counts["Entries with docs"]
        + counts["Components with docs"]
        + counts["Prefabs with docs"]
    )
    return counts


def render_statistics_tree(counts: dict[str, int], include_catalog_total: bool = True) -> str:
    number = lambda label: f"{counts[label]:,}"
    coverage = lambda label, docs_label=None: (
        "documented"
        if counts[label] == 1 and counts[docs_label or f"{label} with docs"] == 1
        else "all documented"
        if counts[docs_label or f"{label} with docs"] == counts[label]
        else "none documented"
        if counts[docs_label or f"{label} with docs"] == 0
        else f"{counts[docs_label or f'{label} with docs']:,} documented"
    )
    lines = [
            f"├── {number('Total entries')} API entries ({coverage('Total entries', 'Entries with docs')})",
            (
                f"│   ├── {number('Type entries')} Entries under {number('Type declarations')} Global Types "
                f"({coverage('Type entries')})"
            ),
            (
                f"│   │   ├── {number('All types with members')} Types with members "
                f"({coverage('All types with members')})"
            ),
            (
                f"│   │   │   ├── {number('Types with members')} Other types "
                f"(Query, Signal, UDim2, etc. | {coverage('Types with members')})"
            ),
            (
                f"│   │   │   │   └── {number('Other type members')} Members "
                f"(Query.With, Signal.Connect, UDim2.Scale, etc. | {coverage('Other type members')})"
            ),
            "│   │   │   ├── The literal `Instance` type (documented)",
            "│   │   │   └── The literal `World` type (documented)",
            (
                f"│   │   └── {number('Types without members')} Types without members "
                f"(AnimLayers, CloudStore, Pair, etc. | {coverage('Types without members')})"
            ),
            (
                f"│   └── {number('Global entries')} Entries under {number('Global declarations')} "
                f"Global Variables/Classes ({coverage('Global entries')})"
            ),
            (
                f"│       ├── {number('All globals with members')} Variables/classes with members "
                f"({coverage('All globals with members')})"
            ),
            (
                f"│       │   ├── {number('Globals with members')} Other variables/classes "
                f"(UDim2, Vector3, task, etc. | {coverage('Globals with members')})"
            ),
            (
                f"│       │   │   └── {number('Other global members')} Members "
                f"(UDim2.new, Vector3.xAxis, task.wait, etc. | {coverage('Other global members')})"
            ),
            "│       │   ├── `Instance` (documented)",
            "│       │   │   ├── 1 Static constructor (`Instance.new` | documented)",
            (
                f"│       │   │   └── {number('Instance type members')} Members from "
                f"the literal `Instance` type above ({coverage('Instance type members')})"
            ),
            (
                f"│       │   │       ├── {number('Instance methods')} Instance methods "
                f"(instance:AddComponent, instance:ApplyImpulse, etc. | {coverage('Instance methods')})"
            ),
            (
                f"│       │   │       └── {number('Instance properties')} Instance properties "
                f"(instance.Character, instance.Activated, etc. | {coverage('Instance properties')})"
            ),
            "│       │   └── `game` (undocumented)",
            f"│       │       ├── {number('Direct game members')} Direct members ({coverage('Direct game members')})",
            (
                f"│       │       │   ├── {number('Game services')} Services "
                f"(game.World, game.Input, etc. | {coverage('Game services')})"
            ),
            (
                f"│       │       │   └── {number('Other game symbols')} Other symbols "
                f"(game.Bool, game.EventTable, game.ToServer, etc. | {coverage('Other game symbols')})"
            ),
            (
                f"│       │       └── {number('Game service members')} Members across the {number('Game services')} services "
                f"({coverage('Game service members')})"
            ),
            (
                f"│       │           ├── {number('Table game service members')} Members under "
                f"{number('Table game services')} table services "
                f"(game.Physics.Raycast, game.Input.Key, etc. | {coverage('Table game service members')})"
            ),
            (
                f"│       │           └── {number('World type members')} Members from the literal `World` type above "
                f"(game.World.Query, game.World.Each, etc. | {coverage('World type members')})"
            ),
            (
                f"│       └── {number('Globals without members')} Variables without members "
                f"(EventTableDump, handles, print, self, tick, warn | {coverage('Globals without members')})"
            ),
            (
                f"├── {number('Components')} Exposed* components "
                f"(Anchored, MultiJump, etc. | {coverage('Components')})"
            ),
            "│   └── * = Luduvo uses more components than this, but those aren't exposed in scripts",
            f"└── {number('Prefabs')} Prefabs (Part, Hotbar, etc. | {coverage('Prefabs')})",
    ]
    if include_catalog_total:
        lines = [
            f"{counts['Catalog entries']:,} total catalog entries ({counts['Catalog entries with docs']:,} documented)",
            "",
            *lines,
        ]
    return "\n".join(lines)


def render_readme_statistics(counts: dict[str, int], luduvo_version: str) -> str:
    tree = render_statistics_tree(counts, include_catalog_total=False)
    return "\n".join(
        [
            README_STATS_START,
            (
                f"As of Luduvo {luduvo_version}, the prefab section of `resources.luduvo.json`, "
                "`globalTypes.luduvo.client.d.luau`, and `globalTypes.luduvo.server.d.luau` files inside Luduvo LSP "
                "are essentially copy/pasted global type files straight from Luduvo itself (you can find them in the "
                "`prefabs/` and `defs/` directory of your Luduvo install's roaming data equivalent respectively). The "
                "components side of `resources.luduvo.json` was dumped from Luduvo's `LuduvoEditor.exe`. Currently, "
                f"these files contain ~{counts['Catalog entries']} entries spanning across:"
            ),
            "",
            "```text",
            tree,
            "```",
            "",
            (
                f"Out of said {counts['Catalog entries']} entries, there are docs for about "
                f"{counts['Catalog entries with docs']} of them."
            ),
            README_STATS_END,
        ]
    )


def update_readme(path: Path, generated: str) -> None:
    source = path.read_text(encoding="utf-8")
    start = source.find(README_STATS_START)
    end = source.find(README_STATS_END)
    if start < 0 or end < 0 or end < start:
        raise ValueError(f"{path} must contain one valid Luduvo API statistics marker pair")
    end += len(README_STATS_END)
    path.write_text(source[:start] + generated + source[end:], encoding="utf-8", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "api_json",
        nargs="?",
        type=Path,
        default=DEFAULT_API_PATH,
        help=f"API catalog to count (default: {DEFAULT_API_PATH})",
    )
    parser.add_argument(
        "--resources-json",
        type=Path,
        default=DEFAULT_RESOURCES_PATH,
        help=f"Resource catalog used for the component count (default: {DEFAULT_RESOURCES_PATH})",
    )
    parser.add_argument("--json", action="store_true", help="Print the counts as JSON")
    parser.add_argument("--update-readme", type=Path, help="Replace the marked statistics block in this README")
    parser.add_argument("--luduvo-version", help="Luduvo release version used with --update-readme")
    args = parser.parse_args()

    counts = collect_counts(args.api_json, args.resources_json)
    if args.update_readme:
        if not args.luduvo_version:
            parser.error("--luduvo-version is required with --update-readme")
        update_readme(args.update_readme, render_readme_statistics(counts, args.luduvo_version))

    if args.json:
        print(json.dumps(counts, indent=2))
    else:
        if hasattr(sys.stdout, "reconfigure"):
            sys.stdout.reconfigure(encoding="utf-8")
        print(render_statistics_tree(counts))


if __name__ == "__main__":
    main()
