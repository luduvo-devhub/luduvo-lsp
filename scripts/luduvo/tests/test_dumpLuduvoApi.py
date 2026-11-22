from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "dumpLuduvoApi.py"
DEFINITIONS = """declare extern type EventTable with
    Push: (self: EventTable) -> ()
end
declare extern type Instance with
    FindFirstChild: (self: Instance, name: string) -> Instance?
    function AddComponent(self, name: "Tool"): ToolComponent
    function AddComponent(self, ...: string | Pair): Component | Instance
    NightSky: string
    LeaderboardColumn: LeaderboardColumnComponent
end
declare extern type LeaderboardColumnComponent with
    Stat: string
end
export type Signal<F = (...any) -> ()> = {
    Connect: (self: Signal<F>, callback: F) -> SignalConnection,
}
type ParamSpec = {
    Default: number?,
}
declare extern type World with
    -- From the declaration
    FindFirstChild: (name: string) -> Instance?
end
declare extern type DynamicMesh with
end
-- From the declaration
declare DynamicMesh: {
    new: () -> DynamicMesh,
}
declare game: {
    EventTable: (name: string) -> EventTable,
    Session: {
        ServerTime: number,
    },
}
"""
REFERENCE_URL = "https://example.com/docs/reference/"


class ApiDumpTests(unittest.TestCase):
    def test_adds_constructor_names_without_overwriting_object_pages(self):
        with patch.object(sys, "path", [str(SCRIPT.parent), *sys.path]):
            from luduvo_website import object_resources

        constructor = {
            "location": "reference/Instance/#new",
            "text": (
                "<p>Effects</p><ul>"
                "<li><code>Part</code> creates a block. <code>Sphere</code> makes a sphere; "
                "<code>WedgePart</code> makes a <code>Wedge</code>.</li>"
                "<li>Any other name returns an empty <code>Instance</code>.</li></ul>"
                "<p>Example</p><pre><code>Instance.new(&quot;Example&quot;)</code></pre>"
            ),
        }
        part = {
            "location": "reference/classes/Part/",
            "text": '<p>Object <code>Instance.new("Part")</code></p><p>A part description.</p>',
        }
        def fetch_object_page(url: str) -> str:
            self.assertEqual(REFERENCE_URL + "classes/Part/", url)
            return '<h2 id="components">Components</h2><table><tbody><tr><td>Position</td></tr></tbody></table>'

        expected = [
            {"name": "Part", "documentation": "A part description.", "notes": ["Starting components: Position."]},
            {"name": "Sphere"},
            {"name": "Wedge"},
            {"name": "WedgePart"},
        ]
        for items in ([constructor, part], [part, constructor]):
            self.assertEqual(expected, object_resources(json.dumps({"items": items}), REFERENCE_URL, fetch_object_page))

    def dump(self, *options: str) -> list[dict]:
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            definitions = output / "fixture.d.luau"
            definitions.write_text(DEFINITIONS, encoding="utf-8")
            index = output / "search.json"
            index.write_text(
                json.dumps(
                    {
                        "items": [
                            {"location": "reference/EventTable/", "title": "EventTable", "text": "<p>System · game.EventTable</p><p>Create a table.</p>"},
                            {"location": "reference/types/EventTable/", "title": "EventTable", "text": "<p>Type</p><p>A network event table.</p>"},
                            {"location": "reference/types/EventTable/#Push", "title": "Push", "text": "<pre><code>Push(...any)</code></pre><p>Push an event.</p>"},
                            {"location": "reference/World/", "title": "World", "text": "<p>System · game.World</p>"},
                            {"location": "reference/World/#FindFirstChild", "title": "FindFirstChild", "text": "<p>From the website.</p>"},
                            {"location": "reference/Instance/", "title": "Instance", "text": "<p>Class · server and client scripts</p>"},
                            {"location": "reference/Instance/#AddComponent", "title": "AddComponent", "text": "Method <pre><code>AddComponent(...string | Pair)</code></pre><p>Add components.</p>"},
                            {"location": "reference/DynamicMesh/", "title": "DynamicMesh", "text": "<p>Library · <code>DynamicMesh</code></p><p>Build a mesh.</p>"},
                            {"location": "reference/LeaderboardColumn/", "title": "LeaderboardColumn", "text": "<p>Class · server and client scripts</p><p>A scoreboard column.</p>"},
                            {"location": "reference/LeaderboardColumn/#Stat", "title": "Stat", "text": "<p>The attribute name.</p>"},
                            {"location": "reference/types/Signal/", "title": "Signal", "text": "<p>Type</p><p>Runs callbacks.</p>"},
                            {"location": "reference/types/Signal/#Connect", "title": "Connect", "text": "<p>Connects a callback.</p>"},
                            {"location": "reference/types/ParamSpec/", "title": "ParamSpec", "text": "<p>Type</p>"},
                            {"location": "reference/types/ParamSpec/#Default", "title": "Default", "text": "<p>Default is the fallback value.</p>"},
                            {"location": "reference/Instance/#FindFirstChild", "title": "FindFirstChild", "text": "<p>Find an <code>Instance</code> &amp; return it.</p>"},
                            {"location": "reference/Instance/#NightSky", "title": "NightSky", "text": "PropertyDeprecated <pre><code>NightSky: string</code></pre><p>Deprecated</p><p>Use <code>Panorama</code>.</p><p>The old texture.</p><p>Default <code>&quot;0&quot;</code></p><p>Effects</p><ul><li>Writes do nothing.</li></ul><p>Example</p><pre><code>print(1 &lt; 2)</code></pre>"},
                            {"location": "reference/Session/", "title": "Session", "text": "<p>System · game.Session</p>"},
                            {"location": "reference/Session/#clock", "title": "Clock", "text": ""},
                            {"location": "reference/Session/#ServerTime", "title": "ServerTime", "text": "<p>The server time.</p>"},
                            {"location": "reference/classes/EventTable/", "title": "EventTable", "text": "<p>Object</p><p>Ignore this object.</p>"},
                        ],
                    }
                ),
                encoding="utf-8",
            )
            subprocess.run(
                [
                    sys.executable, str(SCRIPT),
                    "--server-definitions", str(definitions),
                    "--client-definitions", str(definitions),
                    "--output-dir", str(output),
                    "--reference-url", REFERENCE_URL,
                    "--source", str(index),
                    *options,
                ],
                capture_output=True,
                text=True,
                check=True,
            )
            rows = json.loads((output / "api-docs/en-us/luduvo-api.json").read_text(encoding="utf-8"))
            return rows

    def test_zensical_links_respect_declaration_identity(self):
        rows = {(row["kind"], row["owner"], row["name"]): row for row in self.dump()}
        for key, location, prose in (
            (("global_member", "game", "EventTable"), "EventTable/", "Create a table."),
            (("type", None, "EventTable"), "types/EventTable/", "A network event table."),
            (("type_member", "EventTable", "Push"), "types/EventTable/#Push", "Push an event."),
            (("type_member", "World", "FindFirstChild"), "World/#FindFirstChild", "From the website."),
            (("type_member", "Instance", "FindFirstChild"), "Instance/#FindFirstChild", "Find an `Instance` & return it."),
            (("global", None, "DynamicMesh"), "DynamicMesh/", "Build a mesh."),
            (("type", None, "DynamicMesh"), "DynamicMesh/", "Build a mesh."),
            (("type_member", "LeaderboardColumnComponent", "Stat"), "LeaderboardColumn/#Stat", "The attribute name."),
            (("type_member", "Signal", "Connect"), "types/Signal/#Connect", "Connects a callback."),
            (("global_member", "game.Session", "ServerTime"), "Session/#ServerTime", "The server time."),
        ):
            with self.subTest(key=key):
                self.assertEqual(REFERENCE_URL + location, rows[key]["learn_more_link"])
                self.assertEqual(prose, rows[key]["documentation"])
        self.assertEqual("(self: EventTable) -> ()", rows["type_member", "EventTable", "Push"]["type"])

    def test_no_docs_skips_loading_the_index(self):
        rows = {(row["kind"], row["owner"], row["name"]): row for row in self.dump("--no-docs", "--source", "missing-index.json")}
        self.assertTrue(rows)
        self.assertTrue(all(row["learn_more_link"] is None for row in rows.values()))
        self.assertEqual("From the declaration", rows["type_member", "World", "FindFirstChild"]["documentation"])

    def test_extracts_optional_documentation_sections(self):
        entry = next(row for row in self.dump() if row["name"] == "NightSky")
        self.assertTrue(entry["deprecated"])
        self.assertEqual("Use `Panorama`.", entry["deprecation"])
        self.assertEqual("The old texture.", entry["documentation"])
        self.assertEqual('`"0"`', entry["default"])
        self.assertEqual(["Writes do nothing."], entry["notes"])
        self.assertEqual("print(1 < 2)", entry["code_sample"])
        fallback = next(row for row in self.dump() if row["owner"] == "ParamSpec")
        self.assertEqual("Default is the fallback value.", fallback["documentation"])
        self.assertNotIn("default", fallback)

    def test_keeps_extern_method_overloads_and_adds_website_documentation(self):
        rows = [row for row in self.dump() if row["name"] == "AddComponent"]
        self.assertEqual(2, len(rows))
        self.assertEqual({
            'function AddComponent(self, name: "Tool"): ToolComponent',
            "function AddComponent(self, ...: string | Pair): Component | Instance",
        }, {row["type"] for row in rows})
        for row in rows:
            self.assertEqual(REFERENCE_URL + "Instance/#AddComponent", row["learn_more_link"])
            self.assertEqual("Add components.", row["documentation"])
            self.assertEqual([
                "@luduvo/client/globaltype/Instance.AddComponent",
                "@luduvo/server/globaltype/Instance.AddComponent",
            ], row["documentation_symbols"])


if __name__ == "__main__":
    unittest.main()
