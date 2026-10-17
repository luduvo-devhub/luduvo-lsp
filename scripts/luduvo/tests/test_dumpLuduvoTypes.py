from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[1] / "dumpLuduvoTypes.py"
SPEC = importlib.util.spec_from_file_location("dumpLuduvoTypes", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
types_dump = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(types_dump)


VALID_DEFINITIONS = """declare extern type Instance with
end

declare game: {}
"""


class InstalledDefinitionsTests(unittest.TestCase):
    def make_store(self, surface: str = "server", data: str = VALID_DEFINITIONS):
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name)
        version_root = root / "versions" / "42"
        logical_name = types_dump.SURFACE_FILES[surface]
        definition = version_root / logical_name
        definition.parent.mkdir(parents=True)
        definition.write_text(data, encoding="utf-8")
        (root / "current").write_text("42\n", encoding="utf-8")
        return temporary, root, definition

    def test_reads_current_version_without_requiring_a_manifest(self):
        temporary, root, definition = self.make_store()
        self.addCleanup(temporary.cleanup)

        source, data, version = types_dump.installed_definitions(root, "server")

        self.assertEqual(definition, source)
        self.assertEqual(VALID_DEFINITIONS, data)
        self.assertEqual("42", version)

    def test_rejects_unsafe_current_version(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        (root / "current").write_text("../outside", encoding="utf-8")

        with self.assertRaisesRegex(types_dump.DefinitionsError, "invalid current"):
            types_dump.current_version(root)

    def test_selects_client_surface(self):
        temporary, root, definition = self.make_store("client")
        self.addCleanup(temporary.cleanup)

        source, _, _ = types_dump.installed_definitions(root, "client")

        self.assertEqual(definition, source)

    def test_default_outputs_have_distinct_server_and_client_snapshots(self):
        self.assertNotEqual(types_dump.OUTPUTS["server"], types_dump.OUTPUTS["client"])
        self.assertEqual(
            "globalTypes.luduvo.server.d.luau",
            types_dump.OUTPUTS["server"].name,
        )
        self.assertEqual(
            "globalTypes.luduvo.client.d.luau",
            types_dump.OUTPUTS["client"].name,
        )
if __name__ == "__main__":
    unittest.main()
