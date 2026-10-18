from __future__ import annotations

import base64
import importlib.util
import io
import json
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock


SCRIPT_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPT_DIR))
MODULE_PATH = SCRIPT_DIR / "dumpLuduvoResources.py"
SPEC = importlib.util.spec_from_file_location("dumpLuduvoResources", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
resources_dump = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(resources_dump)


def component_editor_fixture(name: str, description: str) -> bytes:
    image_base = 0x140000000
    pe_offset = 0x80
    optional_size = 0xF0
    raw_offset = 0x400
    virtual_address = 0x1000
    data = bytearray(raw_offset + 0x1000)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, pe_offset)
    data[pe_offset : pe_offset + 4] = b"PE\0\0"
    struct.pack_into("<H", data, pe_offset + 6, 1)
    struct.pack_into("<H", data, pe_offset + 20, optional_size)
    optional_offset = pe_offset + 24
    struct.pack_into("<H", data, optional_offset, 0x20B)
    struct.pack_into("<Q", data, optional_offset + 24, image_base)
    section_offset = optional_offset + optional_size
    data[section_offset : section_offset + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", data, section_offset + 8, 0x1000, virtual_address, 0x1000, raw_offset)

    def add_string(relative: int, value: str) -> int:
        encoded = value.encode("utf-8") + b"\0"
        data[raw_offset + relative : raw_offset + relative + len(encoded)] = encoded
        return image_base + virtual_address + relative

    name_address = add_string(0x100, name)
    category_address = add_string(0x140, "Gameplay")
    description_address = add_string(0x180, description)
    struct.pack_into(
        "<7Q",
        data,
        raw_offset + 0x200,
        name_address,
        category_address,
        0,
        0,
        description_address,
        0,
        0,
    )
    return bytes(data)


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

            names = resources_dump.load_component_names(components, overrides, ())

        self.assertEqual(["Anchored", "Position", "Tool"], names)

    def test_component_snapshot_includes_instance_component_fields(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            components = root / "components.json"
            overrides = root / "overrides.json"
            server = root / "server.luau"
            client = root / "client.luau"
            components.write_text(json.dumps(["Position"]), encoding="utf-8")
            overrides.write_text(json.dumps(["CameraLens"]), encoding="utf-8")
            server.write_text(
                """
declare extern type Other with
    Ignored: IgnoredComponent
end
declare extern type Instance with
    read Tool: ToolComponent
    read ToolState: ToolStateComponent
end
""",
                encoding="utf-8",
            )
            client.write_text(
                """
declare extern type Instance with
    read Tool: ToolComponent
    SomethingElse: string
end
""",
                encoding="utf-8",
            )

            names = resources_dump.load_component_names(
                components, overrides, (server, client)
            )

        self.assertEqual(
            ["CameraLens", "Position", "Tool", "ToolState"], names
        )

    def test_catalog_uses_singular_kind_ids(self):
        catalog = resources_dump.build_catalog("10", ["Part"], ["Position"], {"Position": "World-space position"})

        self.assertEqual(2, catalog["formatVersion"])
        self.assertEqual("10", catalog["luduvoContentVersion"])
        self.assertEqual({"component", "prefab"}, set(catalog["resources"]))
        self.assertEqual(
            [{"name": "Position", "documentation": "World-space position"}],
            catalog["resources"]["component"],
        )

    def test_extracts_component_descriptions_from_editor_metadata(self):
        descriptions = resources_dump.extract_component_descriptions(
            component_editor_fixture("Position", "World-space position"),
            ["Position"],
        )

        self.assertEqual({"Position": "World-space position"}, descriptions)

    def test_downloads_editor_from_release_channel_bundle(self):
        editor = component_editor_fixture("Position", "World-space position")
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w") as bundle:
            bundle.writestr("LuduvoEditor.exe", editor)
        manifest = {
            "platforms": {
                "windows-x86_64": {
                    "bundle": {"url": "https://example.invalid/luduvo.zip"}
                }
            }
        }
        channel = b"\n".join(
            (
                b"LDV1",
                base64.b64encode(b"signature"),
                base64.b64encode(json.dumps(manifest).encode("utf-8")),
            )
        )

        with mock.patch.object(
            resources_dump,
            "fetch_bytes",
            side_effect=[channel, archive.getvalue()],
        ):
            result = resources_dump.remote_editor_binary(
                "https://example.invalid/channel"
            )

        self.assertEqual(editor, result)

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
            ), mock.patch.object(
                resources_dump,
                "remote_editor_binary",
                return_value=component_editor_fixture("Position", "World-space position"),
            ):
                resources_dump.generate_from_manifest("https://example.invalid/manifest", output)

            generated = json.loads(output.read_text(encoding="utf-8"))

        self.assertEqual("51", generated["luduvoContentVersion"])
        self.assertEqual(["Sphere"], generated["resources"]["prefab"])
        self.assertEqual(
            [{"name": "Position", "documentation": "World-space position"}],
            generated["resources"]["component"],
        )


if __name__ == "__main__":
    unittest.main()
