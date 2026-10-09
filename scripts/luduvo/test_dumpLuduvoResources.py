from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
MODULE_PATH = SCRIPT_DIR / "dumpLuduvoResources.py"
SPEC = importlib.util.spec_from_file_location("dumpLuduvoResources", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
resources_dump = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(resources_dump)


class ResourceCatalogTests(unittest.TestCase):
    def test_extracts_only_flat_prefab_ldv_entries_from_manifest(self):
        version, names = resources_dump.prefab_names_from_manifest(
            {
                "version": 47,
                "files": [
                    {"name": "prefabs/Wedge.ldv"},
                    {"name": "prefabs/Character.LDV"},
                    {"name": "prefabs/nested/Hidden.ldv"},
                    {"name": "animations/rig.ldv"},
                    {"name": "prefabs/readme.txt"},
                ],
            }
        )

        self.assertEqual("47", version)
        self.assertEqual(["Character", "Wedge"], names)

    def test_reads_an_existing_empty_prefab_directory_as_empty(self):
        with tempfile.TemporaryDirectory() as temporary:
            content_root = Path(temporary)
            (content_root / "current").write_text("42\n", encoding="utf-8")
            (content_root / "versions" / "42" / "prefabs").mkdir(parents=True)

            version, names = resources_dump.prefab_names_from_content_root(content_root)

        self.assertEqual("42", version)
        self.assertEqual([], names)

    def test_local_prefabs_are_sorted_and_deduplicated_by_exact_name(self):
        with tempfile.TemporaryDirectory() as temporary:
            content_root = Path(temporary)
            directory = content_root / "versions" / "42" / "prefabs"
            directory.mkdir(parents=True)
            (content_root / "current").write_text("42", encoding="utf-8")
            (directory / "Wedge.ldv").write_bytes(b"")
            (directory / "character.LDV").write_bytes(b"")
            (directory / "ignore.txt").write_bytes(b"")

            _, names = resources_dump.prefab_names_from_content_root(content_root)

        self.assertEqual(["character", "Wedge"], names)

    def test_component_snapshot_and_overrides_are_merged_deterministically(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            components = root / "components.json"
            overrides = root / "overrides.json"
            components.write_text(json.dumps(["Position", "Anchored"]), encoding="utf-8")
            overrides.write_text(json.dumps(["Tool", "Position"]), encoding="utf-8")

            names = resources_dump.load_component_names(components, overrides)

        self.assertEqual(["Anchored", "Position", "Tool"], names)

    def test_catalog_uses_singular_kind_ids(self):
        catalog = resources_dump.build_catalog("10", ["Part"], ["Position"])

        self.assertEqual(1, catalog["formatVersion"])
        self.assertEqual("10", catalog["luduvoContentVersion"])
        self.assertEqual({"component", "prefab"}, set(catalog["resources"]))

    def test_remote_manifest_prefabs_are_written_to_the_generated_artifact(self):
        manifest = json.dumps(
            {
                "version": 51,
                "files": [
                    {"name": "prefabs/Sphere.ldv"},
                    {"name": "defs/luduvo.d.luau"},
                ],
            }
        ).encode()
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "resources.luduvo.json"
            with mock.patch.object(resources_dump, "fetch_bytes", return_value=manifest), mock.patch.object(
                resources_dump, "load_component_names", return_value=["Position"]
            ):
                resources_dump.generate_from_manifest("https://example.invalid/manifest", output)

            generated = json.loads(output.read_text(encoding="utf-8"))

        self.assertEqual("51", generated["luduvoContentVersion"])
        self.assertEqual(["Sphere"], generated["resources"]["prefab"])
        self.assertEqual(["Position"], generated["resources"]["component"])


if __name__ == "__main__":
    unittest.main()
