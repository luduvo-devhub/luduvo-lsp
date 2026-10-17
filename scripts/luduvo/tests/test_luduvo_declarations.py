from __future__ import annotations

import sys
import unittest
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPT_DIR))

from luduvo_declarations import merge_surfaces, parse_declarations
from dumpLuduvoDocs import build_documentation


SOURCE = """
declare game: {
    -- physics configuration
    read Physics: {
        -- break the welds inside a sphere and push the bodies out
        -- server-only
        Explode: (position: vector, radius: number, force: number) -> number,
        -- downward acceleration
        -- server-only write
        Gravity: number,
    },
}

declare extern type Instance with
    -- stable numeric identifier
    -- read-only
    Id: number
end
"""


class DeclarationParserTests(unittest.TestCase):
    def test_separates_prose_from_scope_directive(self):
        records = parse_declarations(SOURCE, "server")
        explode = next(record for record in records if record["name"] == "Explode")

        self.assertEqual(
            "break the welds inside a sphere and push the bodies out",
            explode["documentation"],
        )
        self.assertEqual(["server-only"], explode["directives"])
        self.assertNotIn("server-only", explode["documentation"])
        self.assertEqual(
            ["@luduvo/server/global/game.Physics.Explode"],
            explode["documentation_symbols"],
        )

    def test_recognizes_read_and_write_metadata(self):
        records = parse_declarations(SOURCE, "server")
        physics = next(record for record in records if record["name"] == "Physics")
        gravity = next(record for record in records if record["name"] == "Gravity")
        instance_id = next(record for record in records if record["name"] == "Id")

        self.assertTrue(physics["read_only"])
        self.assertEqual("server", gravity["write_scope"])
        self.assertTrue(instance_id["read_only"])

    def test_merge_keeps_surface_specific_documentation_symbols(self):
        server = parse_declarations("declare sharedValue: number", "server")
        client = parse_declarations("declare sharedValue: number", "client")
        merged = merge_surfaces(server, client)

        self.assertEqual("shared", merged[0]["scope"])
        self.assertEqual(["client", "server"], merged[0]["surfaces"])
        self.assertEqual(
            [
                "@luduvo/client/global/sharedValue",
                "@luduvo/server/global/sharedValue",
            ],
            merged[0]["documentation_symbols"],
        )

    def test_merge_preserves_surface_specific_access_metadata(self):
        server = parse_declarations(
            "declare game: {\n    -- server-only write\n    Gravity: number,\n}", "server"
        )
        client = parse_declarations(
            "declare game: {\n    read Gravity: number,\n}", "client"
        )

        gravity = [record for record in merge_surfaces(server, client) if record["name"] == "Gravity"]

        self.assertEqual(2, len(gravity))
        server_gravity = next(record for record in gravity if record["surfaces"] == ["server"])
        client_gravity = next(record for record in gravity if record["surfaces"] == ["client"])
        self.assertFalse(server_gravity["read_only"])
        self.assertTrue(client_gravity["read_only"])
        self.assertEqual(
            ["@luduvo/server/global/game.Gravity"],
            server_gravity["documentation_symbols"],
        )
        self.assertEqual(
            ["@luduvo/client/global/game.Gravity"],
            client_gravity["documentation_symbols"],
        )

        docs = build_documentation(gravity)
        self.assertNotIn(
            "Read only at runtime.",
            docs["@luduvo/server/global/game.Gravity"]["documentation"],
        )
        self.assertIn(
            "Writable only on the server.",
            docs["@luduvo/server/global/game.Gravity"]["documentation"],
        )
        self.assertIn(
            "Read only at runtime.",
            docs["@luduvo/client/global/game.Gravity"]["documentation"],
        )


if __name__ == "__main__":
    unittest.main()
