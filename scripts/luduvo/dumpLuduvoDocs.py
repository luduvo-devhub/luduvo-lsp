"""Generate the documentationFile bundled by the Luduvo platform."""

from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parent / "api-docs" / "en-us"


def description(record: dict) -> str:
    parts = []
    if record.get("documentation", "").strip():
        parts.append(record["documentation"].strip())
    if record.get("scope") == "server":
        parts.append("Server only.")
    elif record.get("scope") == "client":
        parts.append("Client only.")
    if record.get("write_scope") == "server":
        parts.append("Writable only on the server.")
    elif record.get("write_scope") == "client":
        parts.append("Writable only on the client.")
    if record.get("read_only"):
        parts.append("Read only at runtime.")
    return "\n\n".join(parts)


def main() -> None:
    records = json.loads((ROOT / "luduvo-api.json").read_text(encoding="utf-8"))
    docs = {}
    for record in records:
        entry = {
            "documentation": description(record),
            "learn_more_link": record.get("learn_more_link"),
        }
        for symbol in record["documentation_symbols"]:
            docs[symbol] = entry

    output = ROOT / "documentation.luduvo.json"
    output.write_text(
        json.dumps(docs, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(f"Generated {len(docs)} documentation symbols in {output.name}.")


if __name__ == "__main__":
    main()
