"""Generate the documentationFile bundled by the Luduvo platform."""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent / "api-docs" / "en-us"
REFERENCE = "https://docs.luduvo.com/reference"


def description(row):
    text = row["doc"].strip()
    labels = []
    if row["flags"] & 8:
        labels.append("Server only.")
    if row["flags"] & 16:
        labels.append("Read only at runtime.")
    return "\n\n".join(part for part in [text, *labels] if part)


def entry(row, link):
    return {"documentation": description(row), "learn_more_link": link}


def main():
    rows = json.loads((ROOT / "luduvo-api.json").read_text(encoding="utf-8"))
    api_overrides = json.loads(
        (ROOT / "luduvo-api-overrides.json").read_text(encoding="utf-8")
    )
    docs = {}
    event_table_members = {
        "ToServer",
        "ToClients",
        "F32",
        "I32",
        "U8",
        "Bool",
        "Vec3",
        "Color",
        "Entity",
        "Prefab",
        "String",
    }

    for index, row in enumerate(rows, start=1):
        if row["parent"]:
            continue

        name = row["name"]
        kind = row["flags"] & 7
        children = [child for child in rows if child["parent"] == index]
        children.extend(child for child in api_overrides if child["owner"] == name)

        if name == "Event":
            docs["@luduvo/global/game.EventTable"] = entry(row, f"{REFERENCE}/Event")
            docs["@luduvo/globaltype/EventTableFactory"] = entry(row, f"{REFERENCE}/Event")
            continue

        if name in event_table_members:
            docs[f"@luduvo/globaltype/EventTableFactory.{name}"] = entry(row, f"{REFERENCE}/{name}")
            continue

        if kind == 4:
            owner = f"{name}Service"
            root_symbol = f"@luduvo/globaltype/{owner}"
            root_link = f"{REFERENCE}/{name}"
            docs[f"@luduvo/global/game.{name}"] = entry(row, root_link)
        elif kind == 6:
            owner = name
            root_symbol = f"@luduvo/globaltype/{name}"
            root_link = f"{REFERENCE}/types/{name}"
        elif kind in (2, 3, 5) or (kind == 5 and row["type"]):
            owner = name
            root_symbol = f"@luduvo/global/{name}"
            root_link = f"{REFERENCE}/{name}"
        else:
            owner = name
            root_symbol = f"@luduvo/globaltype/{name}"
            root_link = f"{REFERENCE}/{name}"

        docs[root_symbol] = entry(row, root_link)
        for child in children:
            docs[f"{root_symbol}.{child['name']}"] = entry(
                child, f"{root_link}#{child['name']}"
            )

    output = ROOT / "documentation.luduvo.json"
    output.write_text(
        json.dumps(docs, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"Generated {len(docs)} documentation symbols in {output.name}.")


if __name__ == "__main__":
    main()
