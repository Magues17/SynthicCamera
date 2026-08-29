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

# Mean-channel separation below which the target is not meaningfully visible.
# Eyeballed against the fog presets, not derived - retune if the presets change.
LOW_CONTRAST = 8.0


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


def target_contrast(image, box):
    """How far the target's mean colour sits from the background just outside it.

    bbox_valid is a geometric check - it says the projection landed on-image, not
    that anything is visible there. Under heavy fog a vehicle can wash out until it
    is indistinguishable from the sand behind it, and that sample still carries a
    confident label. Training on those teaches noise, so they need flagging.
    """
    x, y, w, h = box["x"], box["y"], box["w"], box["h"]
    if w < 4 or h < 4:
        return 0.0

    inner = image.crop((x, y, x + w, y + h))
    margin = max(w, h) * 0.4
    outer = image.crop((max(0, x - margin), max(0, y - margin),
                        min(image.width, x + w + margin), min(image.height, y + h + margin)))

    inner_mean = [sum(c) / len(c) for c in zip(*inner.get_flattened_data())]
    outer_mean = [sum(c) / len(c) for c in zip(*outer.get_flattened_data())]
    return max(abs(a - b) for a, b in zip(inner_mean, outer_mean))


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
        row["_contrast"] = target_contrast(image, box)

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

    scenes = Counter(r["scene"].get("weather", "unspecified") for r in rows)
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
    print(f"weather        : {dict(scenes)}")

    # Randomisation coverage. A range that has collapsed to a single value means the
    # labels claim variation the images do not have, which is the failure mode worth
    # catching here - it is invisible in any single sample.
    for field, label in (("sun_pitch_deg", "sun elevation"), ("sun_yaw_deg", "sun bearing"),
                         ("sun_intensity", "sun intensity"), ("fog_density", "fog density"),
                         ("ambient_intensity", "ambient")):
        values = [r["scene"][field] for r in rows if field in r["scene"]]
        if not values:
            print(f"{label:14s} : ABSENT - not being recorded")
        elif max(values) - min(values) < 1e-6:
            print(f"{label:14s} : CONSTANT at {values[0]:.3f} - not varying")
        else:
            print(f"{label:14s} : {min(values):.2f} to {max(values):.2f}")

    # Framing check. Camera position, aim direction and the trip plane are set
    # together; if they ever drift apart the boxes stay geometrically valid but march
    # off toward an edge. A centre offset that creeps up is the visible symptom.
    offsets = []
    for r in rows:
        box = r["geometry"].get("bbox_xywh")
        if not box:
            continue
        cx = (box["x"] + box["w"] / 2) / r["image_width"]
        cy = (box["y"] + box["h"] / 2) / r["image_height"]
        offsets.append(max(abs(cx - 0.5), abs(cy - 0.5)) * 2)
    if offsets:
        print(f"framing offset : {min(offsets):.2f} to {max(offsets):.2f} "
              f"(0 = centred, 1 = at frame edge)")

    sizes = [r["geometry"]["bbox_xywh"]["w"] * r["geometry"]["bbox_xywh"]["h"] /
             (r["image_width"] * r["image_height"])
             for r in rows if r["geometry"].get("bbox_xywh")]
    if sizes:
        print(f"target size    : {min(sizes)*100:.1f}% to {max(sizes)*100:.1f}% of frame")

    contrasts = [r["_contrast"] for r in rows if "_contrast" in r]
    if contrasts:
        faint = [r for r in rows if r.get("_contrast", 99) < LOW_CONTRAST]
        print(f"target contrast: {min(contrasts):.1f} to {max(contrasts):.1f}")
        if faint:
            print(f"                 {len(faint)} sample(s) below {LOW_CONTRAST} - target barely "
                  f"separable from background, consider dropping or capping fog:")
            for r in faint:
                print(f"                   {r['image_file']}  {r['scene']['weather']} "
                      f"fog {r['scene'].get('fog_density', 0):.2f}  contrast {r['_contrast']:.1f}")

    print(f"previews       : {preview_dir}")
    for message in problems:
        print(f"PROBLEM        : {message}")


main()
