#!/usr/bin/env python3
"""Resource contract, retained baseline and malformed binary regression tests."""
import hashlib
import struct
import sys
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import generate_ui_resources as generate
from rco import Rco, RcoError, id_hash


def decoded_png(blob):
    # Independently validate chunks and reconstruct raw RGBA samples. No
    # dependency on the generator's encoder or the runtime's PNG decoder.
    assert blob[:8] == b"\x89PNG\r\n\x1a\n"
    offset, compressed, dimensions = 8, bytearray(), None
    while offset < len(blob):
        size = struct.unpack_from(">I", blob, offset)[0]
        kind = blob[offset + 4:offset + 8]
        data = blob[offset + 8:offset + 8 + size]
        assert len(data) == size
        crc = struct.unpack_from(">I", blob, offset + 8 + size)[0]
        assert crc == zlib.crc32(kind + data)
        if kind == b"IHDR":
            width, height, bits, mode, compression, filtering, interlace = struct.unpack(">IIBBBBB", data)
            assert (bits, mode, compression, filtering, interlace) == (8, 6, 0, 0, 0)
            dimensions = width, height
        elif kind == b"IDAT":
            compressed.extend(data)
        elif kind == b"IEND":
            assert size == 0
        else:
            raise AssertionError("Unexpected PNG chunk")
        offset += size + 12
    assert offset == len(blob) and blob[-8:-4] == b"IEND"
    width, height = dimensions
    raw = zlib.decompress(compressed)
    assert len(raw) == height * (1 + width * 4)
    rows = []
    for row in range(height):
        start = row * (1 + width * 4)
        assert raw[start] == 0
        rows.append(raw[start + 1:start + 1 + width * 4])
    return dimensions, rows


class ResourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.base = (ROOT / "resources/base.rco").read_bytes()
        cls.config = generate.configuration()
        cls.output = generate.generate(cls.base, cls.config)
        cls.resource = Rco(cls.output)

    def test_header_is_separate_and_original_labels_keep_their_font(self):
        new = self.resource.find("style_text", self.config["header_style"])
        original = self.resource.find("style_text", "psvs_style_text_impose")
        new_text, old_text = (self.resource.node(node.first) for node in (new, original))
        self.assertEqual(self.resource.value(new_text.attribute("font_size")), 18)
        self.assertEqual(self.resource.value(new_text.attribute("bold")), 0)
        self.assertEqual(self.resource.value(old_text.attribute("font_size")), 24)
        self.assertEqual(self.resource.value(old_text.attribute("bold")), 1)
        self.assertEqual(generate.ids_header(self.config), (ROOT / "src/ui/resource_ids.h").read_text())

    def test_baseline_attributes_files_and_unrelated_links_are_preserved(self):
        before = Rco(self.base)
        parents = {before.find(name).offset for name in ("styletable", "templatetable", "texturetable")}
        old_last = {before.node(parent).last for parent in parents}
        for node in before.validate():
            after = self.resource.node(node.offset)
            self.assertEqual((node.name, node.attributes, node.parent, node.previous, node.first),
                             (after.name, after.attributes, after.parent, after.previous, after.first))
            if node.offset not in parents:
                self.assertEqual(node.last, after.last)
            if node.offset not in old_last:
                self.assertEqual(node.following, after.following)
        for index in (1, 4, 6, 7):
            self.assertEqual(before.tables[index], self.resource.tables[index])
        for index in (2, 3, 5, 8):
            self.assertEqual(before.tables[index], self.resource.tables[index][:len(before.tables[index])])
        self.assertEqual(hashlib.sha256((ROOT / "resources/base.rco").read_bytes()).hexdigest(), generate.BASE_SHA256)

    def test_index_font_is_smaller_without_changing_header_or_list_font(self):
        index = self.resource.find("style_text", self.config["index_style"])
        text = self.resource.node(index.first)
        self.assertEqual(self.resource.value(text.attribute("font_size")), 16)
        self.assertEqual(self.resource.value(text.attribute("bold")), 0)
        for style, points in ((self.config["header_style"], 18), ("psvs_style_text_impose", 24)):
            original = self.resource.find("style_text", style)
            self.assertEqual(self.resource.value(self.resource.node(original.first).attribute("font_size")), points)

    def test_all_switch_planes_bind_expected_shared_textures(self):
        template = self.resource.find("template", self.config["switch_template"])
        group = self.resource.node(template.first)
        self.assertEqual(group.name, "group")
        self.assertEqual(self.resource.value(group.attribute("id")), id_hash(self.config["switch_group"]))
        planes, offset = [], group.first
        while offset != -1:
            plane = self.resource.node(offset)
            self.assertEqual(plane.name, "plane")
            planes.append(plane)
            offset = plane.following
        self.assertEqual(len(planes), 18)
        for index, plane in enumerate(planes):
            row, part = divmod(index, 3)
            self.assertEqual(self.resource.value(plane.attribute("id")), id_hash(f"vco_switch_{row}_{part}"))
            self.assertEqual(self.resource.value(plane.attribute("texture")),
                             id_hash("vco_texture_" + self.config["parts"][part]["name"]))
            self.assertEqual(self.resource.value(plane.attribute("style")), id_hash("psvs_style_plane_icon"))

    def test_rgba_assets_have_transparent_corners_and_distinct_off_fill(self):
        for part in self.config["parts"]:
            texture = self.resource.find("texture", "vco_texture_" + part["name"])
            source = texture.attribute("src")
            self.assertEqual(source.value % 16, 0)
            self.assertEqual(self.resource.value(texture.attribute("type")), "texture/png")
            dimensions, pixels = decoded_png(self.resource.value(source))
            self.assertEqual(dimensions, (part["width"], part["height"]))
            self.assertEqual(pixels[0][3], 0)
            center = pixels[part["height"] // 2][(part["width"] // 2) * 4 + 3]
            self.assertEqual(center, 64 if part["outline"] else 255)
            self.assertTrue(any(0 < row[at] < 255 for row in pixels for at in range(3, len(row), 4)))

    def test_generation_is_deterministic_and_rejects_changed_baseline(self):
        self.assertEqual(generate.generate(self.base, self.config), self.output)
        changed = bytearray(self.base)
        changed[-1] ^= 1
        with self.assertRaises(RcoError):
            generate.generate(bytes(changed), self.config)

    def test_bad_extents_cycles_and_links_are_rejected(self):
        original = Rco(self.output)
        tree = struct.unpack_from("<I", self.output, 8)[0]
        variants = []
        for location, value in ((8, 81), (12, 0xFFFFFFFF), (24, 80),
                                (tree + 4, 0x7FFFFFFF), (tree + 20, 0),
                                (tree + original.node(0).first + 8, -1)):
            data = bytearray(self.output)
            struct.pack_into("<I", data, location, value & 0xFFFFFFFF)
            variants.append(data)
        for data in variants:
            with self.subTest(location=variants.index(data)), self.assertRaises(RcoError):
                Rco(bytes(data))


if __name__ == "__main__":
    unittest.main()
