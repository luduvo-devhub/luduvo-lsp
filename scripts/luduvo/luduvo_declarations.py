"""Extract API records and declaration comments from Luduvo Luau definitions."""

from __future__ import annotations

import re
from dataclasses import dataclass, field


COMMENT = re.compile(r"^\s*--\s?(.*)$")
EXTERN = re.compile(r"^\s*declare\s+extern\s+type\s+([A-Za-z_]\w*)\s+with\s*$")
TABLE_TYPE = re.compile(r"^\s*(?:export\s+)?type\s+([A-Za-z_]\w*)\s*=\s*\{\s*$")
GLOBAL_TABLE = re.compile(r"^\s*declare\s+([A-Za-z_]\w*)\s*:\s*\{\s*$")
GLOBAL_VALUE = re.compile(r"^\s*declare\s+([A-Za-z_]\w*)\s*:\s*(.+?)\s*$")
FIELD = re.compile(r"^\s*(read\s+)?([A-Za-z_]\w*)\s*:\s*(.+?)\s*,?\s*$")
NESTED_TABLE = re.compile(r"^\s*(read\s+)?([A-Za-z_]\w*)\s*:\s*\{\s*$")
DIRECTIVES = {"server-only", "client-only", "read-only"}


@dataclass
class Context:
    path: str
    closing: str
    kind: str
    indent: int


@dataclass
class CommentBlock:
    prose: list[str] = field(default_factory=list)
    directives: set[str] = field(default_factory=set)
    write_scope: str | None = None


def _comments(lines: list[str]) -> CommentBlock:
    result = CommentBlock()
    for text in lines:
        normalized = text.strip().lower()
        if normalized in DIRECTIVES:
            result.directives.add(normalized)
        elif normalized == "server-only write":
            result.write_scope = "server"
        elif normalized == "client-only write":
            result.write_scope = "client"
        else:
            result.prose.append(text.strip())
    return result


def _symbol(package: str, kind: str, owner: str | None, name: str) -> str:
    if kind in {"type", "type_member"}:
        path = name if owner is None else f"{owner}.{name}"
        return f"{package}/globaltype/{path}"
    path = name if owner is None else f"{owner}.{name}"
    return f"{package}/global/{path}"


def parse_declarations(source: str, surface: str) -> list[dict]:
    """Return declaration-authoritative API records for one surface."""
    records: list[dict] = []
    stack: list[Context] = []
    pending: list[str] = []
    package = f"@luduvo/{surface}"

    def add(name: str, owner: str | None, kind: str, type_text: str, read: bool = False) -> None:
        comments = _comments(pending)
        records.append(
            {
                "name": name,
                "owner": owner,
                "kind": kind,
                "type": type_text.rstrip(","),
                "documentation": "\n".join(part for part in comments.prose if part),
                "directives": sorted(comments.directives),
                "read_only": read or "read-only" in comments.directives,
                "write_scope": comments.write_scope,
                "surfaces": [surface],
                "documentation_symbols": [_symbol(package, kind, owner, name)],
                "learn_more_link": None,
            }
        )
        pending.clear()

    for line in source.splitlines():
        comment = COMMENT.match(line)
        if comment:
            pending.append(comment.group(1))
            continue
        if not line.strip():
            pending.clear()
            continue

        stripped = line.strip()
        indent = len(line) - len(line.lstrip(" \t"))
        if stack and ((stack[-1].closing == "end" and stripped == "end") or (stack[-1].closing == "}" and stripped.startswith("}"))):
            stack.pop()
            pending.clear()
            continue

        match = EXTERN.match(line)
        if match:
            name = match.group(1)
            add(name, None, "type", f"extern type {name}")
            stack.append(Context(name, "end", "type", indent))
            continue
        match = TABLE_TYPE.match(line)
        if match:
            name = match.group(1)
            add(name, None, "type", f"type {name}")
            stack.append(Context(name, "}", "type", indent))
            continue
        match = GLOBAL_TABLE.match(line)
        if match:
            name = match.group(1)
            add(name, None, "global", "table")
            stack.append(Context(name, "}", "global", indent))
            continue
        match = GLOBAL_VALUE.match(line)
        if match and not stack:
            add(match.group(1), None, "global", match.group(2))
            continue

        match = NESTED_TABLE.match(line)
        if match and stack:
            read, name = match.groups()
            parent = stack[-1]
            kind = "type_member" if parent.kind == "type" else "global_member"
            add(name, parent.path, kind, "table", bool(read))
            stack.append(Context(f"{parent.path}.{name}", "}", parent.kind, indent))
            continue
        match = FIELD.match(line)
        if match and stack:
            read, name, type_text = match.groups()
            parent = stack[-1]
            kind = "type_member" if parent.kind == "type" else "global_member"
            add(name, parent.path, kind, type_text, bool(read))
            continue

        # A non-comment declaration separator breaks comment adjacency.
        pending.clear()

    return records


def merge_surfaces(server: list[dict], client: list[dict]) -> list[dict]:
    """Merge equivalent declarations while retaining per-surface symbols."""
    merged: dict[tuple[str | None, str, str], dict] = {}
    for record in [*server, *client]:
        key = (record["owner"], record["name"], record["kind"])
        existing = merged.get(key)
        if existing is None:
            merged[key] = record.copy()
            continue
        existing["surfaces"] = sorted(set(existing["surfaces"] + record["surfaces"]))
        existing["documentation_symbols"] = sorted(
            set(existing["documentation_symbols"] + record["documentation_symbols"])
        )
        existing["directives"] = sorted(set(existing["directives"] + record["directives"]))
        existing["read_only"] = existing["read_only"] or record["read_only"]
        if not existing["documentation"]:
            existing["documentation"] = record["documentation"]
        if not existing["type"]:
            existing["type"] = record["type"]

    for record in merged.values():
        directives = set(record["directives"])
        if "server-only" in directives or record["surfaces"] == ["server"]:
            record["scope"] = "server"
        elif "client-only" in directives or record["surfaces"] == ["client"]:
            record["scope"] = "client"
        else:
            record["scope"] = "shared"
    return sorted(merged.values(), key=lambda row: (row["owner"] or "", row["name"], row["kind"]))
