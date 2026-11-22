from __future__ import annotations

import json
import re
from html import unescape
from html.parser import HTMLParser
from urllib.parse import urljoin


class WebsiteIndexError(RuntimeError):
    """Raised when the website search index cannot be used."""


class Paragraphs(HTMLParser):
    def __init__(self, source: str):
        super().__init__()
        self.paragraphs: list[str] = []
        self.parts: list[str] | None = None
        self.feed(source)
        self.close()

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag == "p":
            self.parts = []
        elif tag == "code" and self.parts is not None:
            self.parts.append("`")

    def handle_endtag(self, tag: str) -> None:
        if self.parts is None:
            return
        if tag == "p":
            self.paragraphs.append(" ".join("".join(self.parts).split()))
            self.parts = None
        elif tag == "code":
            self.parts.append("`")

    def handle_data(self, data: str) -> None:
        if self.parts is not None:
            self.parts.append(data)


def effect_items(body: str) -> list[str]:
    effects = re.search(r"<p>Effects</p>\s*<ul>(.*?)</ul>", body, re.DOTALL)
    return re.findall(r"<li>(.*?)</li>", effects[1], re.DOTALL) if effects else []


def object_resources(source: str, reference_url: str, fetch_text) -> list[dict]:
    objects = {}
    for item in json.loads(source)["items"]:
        if not isinstance(item, dict):
            continue
        location, body = item.get("location"), item.get("text")
        if not isinstance(location, str) or not isinstance(body, str):
            continue
        if location == "reference/Instance/#new":
            for item in effect_items(body):
                # Constructor lists start with a code-formatted name. The fallback
                # prose and copyable examples can mention unrelated identifiers.
                if re.match(r"\s*<code>[A-Z][A-Za-z0-9_]*</code>", item):
                    for name in re.findall(r"<code>([A-Z][A-Za-z0-9_]*)</code>", item):
                        objects.setdefault(name, {"name": name})
            continue
        if not re.fullmatch(r"reference/classes/[A-Za-z0-9_-]+/", location):
            continue
        paragraphs = Paragraphs(body).paragraphs
        constructor = re.search(r'Instance\.new\("([^"]+)"\)', paragraphs[0] if paragraphs else "")
        if not constructor:
            continue
        name = constructor[1]
        page = fetch_text(urljoin(reference_url.rstrip("/") + "/", location.removeprefix("reference/")))
        components = re.search(r'<h2 id="components"[^>]*>.*?</h2>.*?<tbody>(.*?)</tbody>', page, re.DOTALL)
        entry = {"name": name}
        if len(paragraphs) > 1:
            entry["documentation"] = paragraphs[1]
        if components:
            names = [unescape(re.sub(r"<[^>]+>", "", cell)).strip()
                     for cell in re.findall(r"<tr>\s*<td>(.*?)</td>", components[1], re.DOTALL)]
            if names:
                entry["notes"] = ["Starting components: " + ", ".join(names) + "."]
        objects[name] = entry
    return [objects[name] for name in sorted(objects)]


def reference_entries(source: str) -> dict[str, str]:
    try:
        data = json.loads(source)
    except json.JSONDecodeError as error:
        raise WebsiteIndexError(f"invalid website search index JSON: {error}") from error
    if not isinstance(data, dict) or not isinstance(data.get("items"), list):
        raise WebsiteIndexError("website search index has no items array")

    entries = {}
    for item in data["items"]:
        if not isinstance(item, dict):
            continue
        location, title, body = (item.get(key) for key in ("location", "title", "text"))
        if not all(isinstance(value, str) for value in (location, title, body)):
            continue
        if not location.startswith("reference/"):
            continue
        path = location.removeprefix("reference/")
        if not path or path.startswith(("#", "pages/", "classes/")):
            continue
        if "#" in path and path.partition("#")[2] != title:
            continue  # Section headings use slug anchors. API members use their names.
        entries[path] = body

    if not entries:
        raise WebsiteIndexError("website search index has no API reference entries")
    return entries


def summary(paragraphs: list[str], page: bool, deprecation: str | None) -> str:
    if page:
        paragraphs = paragraphs[1:]  # API pages start with a kind and access label.
    for paragraph in paragraphs:
        if paragraph in {"Effects", "Example", "Examples"}:
            break
        if paragraph and paragraph not in {"Deprecated", deprecation} and not paragraph.startswith("Default `"):
            return paragraph
    return ""


def documented_details(body: str, paragraphs: list[str]) -> dict:
    details = {}
    badge_text, signature, _ = body.partition("<pre>")
    if signature and badge_text.rstrip().endswith("Deprecated"):
        details["deprecated"] = True
        if "Deprecated" in paragraphs:
            index = paragraphs.index("Deprecated") + 1
            if index < len(paragraphs) and paragraphs[index] not in {"Example", "Effects"}:
                details["deprecation"] = paragraphs[index]
    default = next((p.removeprefix("Default ") for p in paragraphs if p.startswith("Default `")), None)
    if default:
        details["default"] = default
    if notes := effect_items(body):
        details["notes"] = [
            paragraph
            for item in notes
            for paragraph in Paragraphs(f"<p>{item}</p>").paragraphs
            if paragraph
        ]
    example = re.search(r"<p>Examples?</p>\s*<pre><code>(.*?)</code></pre>", body, re.DOTALL)
    if example:
        details["code_sample"] = unescape(example[1]).strip()
    return details


def enrich_from_website(api: list[dict], source: str, reference_url: str) -> None:
    """Prefer website prose and metadata for matching declaration symbols."""
    entries = reference_entries(source)
    type_pages = {path.removeprefix("types/").rstrip("/") for path in entries if path.startswith("types/") and "#" not in path}
    system_pages = {
        path.rstrip("/")
        for path, body in entries.items()
        if "#" not in path and body.startswith(("<p>System ", "<p>Field of "))
    }
    class_pages = {
        path.rstrip("/")
        for path, body in entries.items()
        if "#" not in path and body.startswith("<p>Class ")
    }
    library_pages = {
        path.rstrip("/")
        for path, body in entries.items()
        if "#" not in path and body.startswith("<p>Library ")
    }
    component_pages = {
        row["type"]: row["name"]
        for row in api
        if row["owner"] == "Instance" and row["name"] in class_pages and row["type"] == row["name"] + "Component"
    }
    reference_base = reference_url.rstrip("/") + "/"

    for record in api:
        kind, owner, name = (record[key] for key in ("kind", "owner", "name"))
        if kind == "type":
            if name in type_pages:
                path = f"types/{name}/"
            elif name in component_pages:
                path = f"{component_pages[name]}/"
            elif name in class_pages or name in system_pages or name in library_pages:
                path = f"{name}/"
            else:
                continue
        elif kind == "type_member":
            if owner == "Enums":
                path = f"Enum/#{name}"
            elif owner in type_pages:
                path = f"types/{owner}/#{name}"
            elif owner in component_pages:
                path = f"{component_pages[owner]}/#{name}"
            elif owner in class_pages or owner in system_pages:
                path = f"{owner}/#{name}"
            else:
                continue
        elif kind == "global":
            if name in system_pages or (name in class_pages and name != "Instance"):
                continue
            path = f"{name}/"
        elif owner == "game":
            if name in system_pages:
                path = f"{name}/"
            elif record["type"] in {"EventFieldType", "EventDirection"}:
                # The declaration reader flattens members of the EventTable intersection.
                path = f"EventTable/#{name}"
            else:
                continue
        else:
            path = f"{owner.removeprefix('game.')}/#{name}"

        body = entries.get(path)
        if body is None:
            continue
        record["learn_more_link"] = urljoin(reference_base, path)
        paragraphs = Paragraphs(body).paragraphs
        record.update(documented_details(body, paragraphs))
        website_summary = summary(paragraphs, "#" not in path, record.get("deprecation"))
        if website_summary:
            record["documentation"] = website_summary
