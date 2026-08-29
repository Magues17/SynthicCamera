"""
Draw each label back onto its own image.

This is the only check that the whole chain is honest: the bounding box is computed
from 3D bounds and a projection matrix, never from the pixels, so if the maths is
wrong the numbers still look perfectly reasonable in the JSONL. Overlaying them is
what makes a bad box obvious.

    python tools/preview_labels.py Saved/SynthData/<run>

Writes annotated copies to <run>/preview/ and prints a summary.
"""

import json
import sys
from collections import Counter
from pathlib import Path

from PIL import Image, ImageDraw

BOX_COLOUR = (0, 255, 128)
CLIPPED_COLOUR = (255, 190, 0)


def load_rows(labels_path):
    rows = []
    for line_number, line in enumerate(labels_path.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError as error:
            # A truncated final line means the run was killed mid-write. Say so rather
            # than silently training on a short dataset.
            raise SystemExit(f"{labels_path}:{line_number} is not valid JSON: {error}")
    return rows


def annotate(row, run_dir, preview_dir):
    image_path = run_dir / row["image_file"]
    if not image_path.exists():
        return f"missing image: {row['image_file']}"

    image = Image.open(image_path).convert("RGB")
    draw = ImageDraw.Draw(image)
    geometry = row["geometry"]
    box = geometry.get("bbox_xywh")

    if box:
        colour = CLIPPED_COLOUR if geometry.get("bbox_clipped") else BOX_COLOUR
        draw.rectangle(
            [box["x"], box["y"], box["x"] + box["w"], box["y"] + box["h"]],
            outline=colour, width=3)

    vehicle = row["vehicle"]
    kinematics = row["kinematics"]
    caption = (f'{vehicle["class"]} {vehicle["model"]}  '
               f'{kinematics["speed_kph_true"]:.1f} km/h true / '
               f'{kinematics["speed_kph_radar"]:.1f} radar  '
               f'({kinematics["cosine_angle_deg"]:.0f} deg off-axis)')
    draw.text((10, 10), caption, fill=(255, 255, 255))

    image.save(preview_dir / f'preview_{row["image_file"]}')
    return None


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)

    run_dir = Path(sys.argv[1])
    labels_path = run_dir / "labels.jsonl"
    if not labels_path.exists():
        raise SystemExit(f"no labels.jsonl in {run_dir}")

    rows = load_rows(labels_path)
    preview_dir = run_dir / "preview"
    preview_dir.mkdir(exist_ok=True)

    problems = [message for message in (annotate(r, run_dir, preview_dir) for r in rows) if message]

    classes = Counter(r["vehicle"]["class"] for r in rows)
    valid = sum(1 for r in rows if r["geometry"]["bbox_valid"])
    clipped = sum(1 for r in rows if r["geometry"]["bbox_clipped"])
    speeds = [r["kinematics"]["speed_kph_true"] for r in rows]

    print(f"samples        : {len(rows)}")
    print(f"bbox valid     : {valid}/{len(rows)}")
    print(f"bbox clipped   : {clipped}")
    if speeds:
        print(f"speed range    : {min(speeds):.1f} - {max(speeds):.1f} km/h")
    print(f"class balance  : {dict(classes)}")
    print(f"previews       : {preview_dir}")
    for message in problems:
        print(f"PROBLEM        : {message}")


main()
