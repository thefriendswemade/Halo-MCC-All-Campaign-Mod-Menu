"""Builds the killstreak icon font.

Downloads the Game-icons.net SVGs listed below (CC BY 3.0, see ICON_CREDITS.txt),
turns each into a glyph of one small TrueType font, and writes:
  src/ui/icon_font.cpp   the font as a byte array (compiled into the DLL)
  src/ui/icon_codes.h    killstreak id -> the character that draws it
  tools/icons/preview.png  a contact sheet to eyeball the result

Run:  python tools/icons/build_icons.py        (needs: pip install fonttools pillow)
Add or change an icon by editing ICONS; the SVGs are cached in tools/icons/svg/.
"""
import math
import re
import sys
import urllib.request
from pathlib import Path

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.svgLib.path import parse_path

ROOT = Path(__file__).resolve().parents[2]
SVG_DIR = Path(__file__).parent / "svg"
RAW = "https://raw.githubusercontent.com/game-icons/icons/master/"
FIRST_CODEPOINT = 0xE000

# id (matches kStreaks in overlay.cpp) -> layers drawn on a 512x512 canvas.
# A layer is (source, scale, dx, dy); source is a game-icons path or "custom:<name>".
ICONS = {
    "nuke": [("lorc/mushroom-cloud", 1, 0, 0)],
    "upgradeAi": [("skoll/rank-3", 1, 0, 0)],
    "despawnAi": [("lorc/ghost", 1, 0, 0)],
    "randomTeams": [("delapouite/perspective-dice-six-faces-random", 1, 0, 0)],
    "randomSwap": [("lorc/cycle", 1, 0, 0)],
    "grenades": [("lorc/grenade", 1, 0, 0)],
    "bombingRun": [("lorc/bombing-run", 1, 0, 0)],
    "saveCheckpoint": [("delapouite/flag-objective", 1, 0, 0)],
    "loadCheckpoint": [("delapouite/backward-time", 1, 0, 0)],
    "upgradeVehicle": [("delapouite/jeep", 0.8, 0, 110), ("badges/arrow-up", 0.85, 270, -50)],
    "giveWeapon": [("skoll/winchester-rifle", 1, 0, 0)],
    "giveEquipment": [("delapouite/backpack", 1, 0, 0)],
    "spawn": [("custom:spawn", 1, 0, 0)],
    "takeSeat": [("delapouite/plane-pilot", 1, 0, 0)],
    "teleport": [("lorc/teleport", 1, 0, 0)],
}


def fetch(path):
    file = SVG_DIR / (path.replace("/", "__") + ".svg")
    if not file.exists():
        SVG_DIR.mkdir(exist_ok=True)
        with urllib.request.urlopen(RAW + path + ".svg") as r:
            file.write_bytes(r.read())
    return file.read_text(encoding="utf-8")


def svg_paths(text):
    """The drawn paths of a game-icons SVG (the black background square is skipped)."""
    out = []
    for m in re.finditer(r'<path\b[^>]*?\bd="([^"]+)"', text):
        d = m.group(1)
        if re.fullmatch(r"\s*M\s*0\s*0\s*h\s*512\s*v\s*512\s*H\s*0\s*z\s*", d):
            continue
        out.append(d)
    return out


def custom_spawn(pen):
    """A plus inside a dashed ring (y down, 512 canvas)."""
    cx = cy = 256.0
    r_out, r_in = 232.0, 192.0
    for k in range(8):
        a0 = math.radians(k * 45 + 4)
        a1 = math.radians(k * 45 + 41)
        steps = 6
        pts = [(cx + r_out * math.cos(a0 + (a1 - a0) * i / steps), cy + r_out * math.sin(a0 + (a1 - a0) * i / steps))
               for i in range(steps + 1)]
        pts += [(cx + r_in * math.cos(a1 - (a1 - a0) * i / steps), cy + r_in * math.sin(a1 - (a1 - a0) * i / steps))
                for i in range(steps + 1)]
        pen.moveTo(pts[0])
        for p in pts[1:]:
            pen.lineTo(p)
        pen.closePath()
    h, t = 104.0, 24.0
    plus = [(-t, -h), (t, -h), (t, -t), (h, -t), (h, t), (t, t), (t, h), (-t, h), (-t, t), (-h, t), (-h, -t), (-t, -t)]
    pen.moveTo((cx + plus[0][0], cy + plus[0][1]))
    for x, y in plus[1:]:
        pen.lineTo((cx + x, cy + y))
    pen.closePath()


def draw_layer(pen, source, scale, dx, dy):
    # Canvas (y down) to font units (y up, 1024 per em, 128 below the baseline).
    k = 2.0
    flip = (k * scale, 0, 0, -k * scale, k * dx, 896 - k * dy)
    tp = TransformPen(pen, flip)
    if source == "custom:spawn":
        custom_spawn(tp)
        return
    for d in svg_paths(fetch(source)):
        parse_path(d, tp)


def main():
    glyph_order = [".notdef"] + [f"icon_{i}" for i in ICONS]
    glyphs = {}
    notdef = TTGlyphPen(None)
    glyphs[".notdef"] = notdef.glyph()
    cmap = {}
    for index, (name, layers) in enumerate(ICONS.items()):
        pen = TTGlyphPen(None)
        quad = Cu2QuPen(pen, max_err=1.0, reverse_direction=False)
        for source, scale, dx, dy in layers:
            draw_layer(quad, source, scale, dx, dy)
        glyphs[f"icon_{name}"] = pen.glyph()
        cmap[FIRST_CODEPOINT + index] = f"icon_{name}"

    fb = FontBuilder(1024, isTTF=True)
    fb.setupGlyphOrder(glyph_order)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics({g: (1024, 0) for g in glyph_order})
    fb.setupHorizontalHeader(ascent=896, descent=-128)
    fb.setupNameTable({"familyName": "HaloModMenuIcons", "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=896, sTypoDescender=-128, usWinAscent=896, usWinDescent=128)
    fb.setupPost()
    font_path = Path(__file__).parent / "icons.ttf"
    fb.save(str(font_path))
    data = font_path.read_bytes()
    print(f"{len(ICONS)} icons, {len(data)} bytes")

    lines = ["// Generated by tools/icons/build_icons.py. Do not edit.", "#include \"icon_codes.h\"", "",
             "const unsigned int kIconFontSize = %d;" % len(data), "const unsigned char kIconFontData[] = {"]
    for i in range(0, len(data), 24):
        lines.append("    " + ",".join(str(b) for b in data[i:i + 24]) + ",")
    lines.append("};")
    (ROOT / "src/ui/icon_font.cpp").write_text("\n".join(lines) + "\n", encoding="utf-8")

    header = ["// Generated by tools/icons/build_icons.py. Do not edit.", "#pragma once", "",
              "// Killstreak id -> the icon font character that draws it (UTF-8).", "struct IconCode { const char* id; const char* utf8; };",
              "extern const unsigned int kIconFontSize;", "extern const unsigned char kIconFontData[];",
              "constexpr unsigned short kIconFirst = 0x%04X, kIconLast = 0x%04X;" % (FIRST_CODEPOINT, FIRST_CODEPOINT + len(ICONS) - 1),
              "constexpr IconCode kIconCodes[] = {"]
    for index, name in enumerate(ICONS):
        cp = FIRST_CODEPOINT + index
        utf8 = chr(cp).encode("utf-8")
        header.append('    {"%s", "%s"},' % (name, "".join("\\x%02x" % b for b in utf8)))
    header.append("};")
    (ROOT / "src/ui/icon_codes.h").write_text("\n".join(header) + "\n", encoding="utf-8")

    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        print("pillow missing: no preview")
        return
    cell = 190
    cols = 5
    rows = (len(ICONS) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cell, rows * cell), (24, 28, 36))
    big = ImageFont.truetype(str(font_path), 128)
    small = ImageFont.truetype("arial.ttf", 14) if Path("C:/Windows/Fonts/arial.ttf").exists() else None
    d = ImageDraw.Draw(sheet)
    for index, name in enumerate(ICONS):
        x, y = (index % cols) * cell, (index // cols) * cell
        d.text((x + cell // 2, y + 8), chr(FIRST_CODEPOINT + index), font=big, fill=(255, 210, 80), anchor="mt")
        d.text((x + cell // 2, y + cell - 12), name, font=small, fill=(200, 200, 200), anchor="mm")
    sheet.save(Path(__file__).parent / "preview.png")
    print("wrote tools/icons/preview.png")


if __name__ == "__main__":
    sys.exit(main())
