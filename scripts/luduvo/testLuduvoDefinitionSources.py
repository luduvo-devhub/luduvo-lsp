from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("dumpLuduvoTypes.py")
SPEC = importlib.util.spec_from_file_location("dumpLuduvoTypes", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
types_dump = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(types_dump)


VALID_DEFINITIONS = """declare extern type Instance with
end

declare game: {}
"""


class InstalledDefinitionsTests(unittest.TestCase):
    def make_store(self, surface: str = "server"):
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name)
        definition = root / "versions" / "42" / types_dump.SURFACE_FILES[surface]
        definition.parent.mkdir(parents=True)
        definition.write_text(VALID_DEFINITIONS, encoding="utf-8")
        (root / "current").write_text("42\n", encoding="utf-8")
        return temporary, root, definition

    def test_reads_the_installed_current_version(self):
        temporary, root, definition = self.make_store()
        self.addCleanup(temporary.cleanup)

        source, data, version = types_dump.installed_definitions(root, "server")

        self.assertEqual(definition, source)
        self.assertEqual(VALID_DEFINITIONS, data)
        self.assertEqual("42", version)

    def test_rejects_an_unsafe_current_version(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        (root / "current").write_text("../outside", encoding="utf-8")

        with self.assertRaisesRegex(types_dump.DefinitionsError, "invalid current"):
            types_dump.current_version(root)


class RemoteDefinitionsTests(unittest.TestCase):
    manifest_url = "https://example.invalid/content/manifest"
    content_base_url = "https://cdn.example.invalid/content/"
    server_hash = "a" * 64
    client_hash = "b" * 64

    def manifest(self, files=None):
        return json.dumps(
            {
                "version": 47,
                "files": files
                if files is not None
                else [
                    {
                        "name": types_dump.SURFACE_FILES["server"],
                        "sha256": self.server_hash,
                    },
                    {"name": "unrelated/file.txt", "sha256": "c" * 64},
                    {
                        "name": types_dump.SURFACE_FILES["client"],
                        "sha256": self.client_hash,
                    },
                ],
            }
        ).encode()

    def test_downloads_both_exact_definition_entries(self):
        responses = {
            self.manifest_url: self.manifest(),
            self.content_base_url + self.server_hash: b"declare game: {}\n",
            self.content_base_url + self.client_hash: b"declare game: {}\n",
        }

        with mock.patch.object(
            types_dump, "fetch_bytes", side_effect=lambda url: responses[url]
        ):
            definitions = types_dump.remote_definitions(
                self.manifest_url, self.content_base_url
            )

        self.assertEqual("47", definitions["server"][2])
        self.assertEqual(
            self.content_base_url + self.server_hash, definitions["server"][0]
        )
        self.assertEqual("declare game: {}\n", definitions["client"][1])

    def test_rejects_a_missing_surface(self):
        files = [
            {
                "name": types_dump.SURFACE_FILES["server"],
                "sha256": self.server_hash,
            }
        ]
        with mock.patch.object(
            types_dump, "fetch_bytes", return_value=self.manifest(files)
        ):
            with self.assertRaisesRegex(types_dump.DefinitionsError, "client"):
                types_dump.remote_definitions(
                    self.manifest_url, self.content_base_url
                )

    def test_rejects_an_invalid_content_hash(self):
        files = [
            {
                "name": types_dump.SURFACE_FILES["server"],
                "sha256": "not-a-content-hash",
            },
            {
                "name": types_dump.SURFACE_FILES["client"],
                "sha256": self.client_hash,
            },
        ]
        with mock.patch.object(
            types_dump, "fetch_bytes", return_value=self.manifest(files)
        ):
            with self.assertRaisesRegex(types_dump.DefinitionsError, "invalid sha256"):
                types_dump.remote_definitions(
                    self.manifest_url, self.content_base_url
                )


if __name__ == "__main__":
    unittest.main()
