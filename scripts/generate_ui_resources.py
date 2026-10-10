#!/usr/bin/env python3
"""Derive the VCO RCO and C++ IDs from the immutable, licensed baseline.

PAF's resource/template machinery owns the PNG decoding and surface lifetime.
No Surface::Load, intrusive_ptr ABI or virtual SetTexture call is needed in VCO.
The baseline is never rewritten. --check also rejects stale generated IDs.
"""
import argparse
import hashlib
import json
from pathlib import Path

from rco import Attribute, Rco, RcoError, floating
from ui_assets import png

ROOT = Path(__file__).resolve().parents[1]
BASE_SHA256 = "72285cc9354009fceaf91567431df490744a83f9f581e79c2e3ff940a48f02d6"


def configuration():
    config = json.loads((ROOT / "resources/overlay.json").read_text())
    if config["rows"] != 6 or config["header_points"] != 18 or config["index_points"] != 16:
        raise RcoError("Resource layout must agree with the native page contract")
    if [part["name"] for part in config["parts"]] != ["track_on", "track_off", "thumb"]:
        raise RcoError("Unexpected switch parts")
    for part in config["parts"]:
        width, height, radius = (part[key] for key in ("width", "height", "radius"))
        if not all(isinstance(value, int) for value in (width, height, radius)):
            raise RcoError("Non-integral asset geometry")
        if not 1 <= height <= width <= 128 or radius != height // 2:
            raise RcoError("Invalid switch geometry")
    return config


def ids_header(config):
    lines = ["/* Generated from resources/overlay.json; do not edit. */",
             "#ifndef VCO_RESOURCE_IDS_H", "#define VCO_RESOURCE_IDS_H",
             f'#define VCO_HEADER_STYLE "{config["header_style"]}"',
             f'#define VCO_INDEX_STYLE "{config["index_style"]}"',
             f'#define VCO_SWITCH_TEMPLATE "{config["switch_template"]}"',
             f'#define VCO_SWITCH_GROUP "{config["switch_group"]}"',
             f'#define VCO_RESOURCE_SWITCH_ROWS {config["rows"]}',
             "static const char *const vco_switch_ids[VCO_RESOURCE_SWITCH_ROWS][3] = {"]
    for row in range(config["rows"]):
        lines.append("    {" + ", ".join(f'"vco_switch_{row}_{index}"' for index in range(3)) + "},")
    lines.extend(["};", "#endif", ""])
    return "\n".join(lines)


def generate(base, config):
    if hashlib.sha256(base).hexdigest() != BASE_SHA256:
        raise RcoError("RCO baseline identity changed; review before regenerating")
    resource = Rco(base)
    # Clone rather than mutate the impose style used by existing list labels.
    original = resource.find("style_text", "psvs_style_text_impose")
    style_table = resource.find("styletable")
    text_object = resource.node(original.first)
    for role in ("header", "index"):
        clone = resource.append("style_text", [a for a in original.attributes if a.name != "id"],
                                style_table.offset, config[role + "_style"])
        attrs = [floating("font_size", config[role + "_points"]) if a.name == "font_size" else
                 Attribute("bold", 1, 0) if a.name == "bold" else a for a in text_object.attributes]
        resource.append("textobj", attrs, clone)
    textures = resource.find("texturetable")
    texture_ids = []
    for part in config["parts"]:
        identifier = "vco_texture_" + part["name"]
        texture_ids.append(identifier)
        image = png(part["width"], part["height"], part["radius"], part["outline"])
        resource.tables[8].extend(bytes((-len(resource.tables[8])) % 16))
        offset = len(resource.tables[8])
        resource.tables[8].extend(image)
        texture_type = "texture/png"
        resource.append("texture", [Attribute("src", 8, offset, len(image)),
                        Attribute("type", 3, resource.intern_string(texture_type), len(texture_type))],
                        textures.offset, identifier)
    templates = resource.find("templatetable")
    template = resource.append("template", [resource.hash_attribute("id", config["switch_template"])],
                               templates.offset)
    group = resource.append("group", [resource.hash_attribute("id", config["switch_group"])], template)
    for row in range(config["rows"]):
        for index, identifier in enumerate(texture_ids):
            resource.append("plane", [
                resource.reference("style", "psvs_style_plane_icon"),
                resource.hash_attribute("id", f"vco_switch_{row}_{index}"),
                resource.reference("texture", identifier)], group)
    return resource.encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--build", action="store_true", help="Write RCO; require reviewed IDs already current")
    parser.add_argument("--output", type=Path, default=ROOT / "build/vco-base.rco")
    options = parser.parse_args()
    config = configuration()
    data = generate((ROOT / "resources/base.rco").read_bytes(), config)
    header = ids_header(config)
    resource_path, header_path = options.output, ROOT / "src/ui/resource_ids.h"
    if options.check:
        if not resource_path.exists() or resource_path.read_bytes() != data:
            raise SystemExit("Stale generated RCO: run scripts/generate_ui_resources.py")
        if not header_path.exists() or header_path.read_text() != header:
            raise SystemExit("Stale generated resource IDs: run scripts/generate_ui_resources.py")
        print("RCO, PNG masks and native resource IDs match reviewed inputs.")
    else:
        if options.build and (not header_path.exists() or header_path.read_text() != header):
            raise SystemExit("Stale resource IDs: review inputs, then run scripts/generate_ui_resources.py")
        resource_path.parent.mkdir(parents=True, exist_ok=True)
        resource_path.write_bytes(data)
        if not options.build:
            header_path.write_text(header)
        print(f"Generated {len(data)}-byte RCO and native resource IDs; baseline untouched.")


if __name__ == "__main__":
    main()
