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
OTHER_COLOUR = (120, 170, 255)

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

    for index, obj in enumerate(row.get("objects", [])):
        geometry = obj["geometry"]
        box = geometry.get("bbox_xywh")
        if not box:
            continue

        # The trigger is drawn in the accent colour and everything else dimmer, so a
        # frame that happens to contain four vehicles still shows which one the
        # shutter fired for.
        if obj.get("triggered_capture"):
            colour = CLIPPED_COLOUR if geometry.get("bbox_clipped") else BOX_COLOUR
            width = 3
        else:
            colour = OTHER_COLOUR
            width = 2

        draw.rectangle(
            [box["x"], box["y"], box["x"] + box["w"], box["y"] + box["h"]],
            outline=colour, width=width)

        vehicle = obj["vehicle"]
        kinematics = obj["kinematics"]
        visible = obj.get("visibility", {}).get("visible_fraction", 1.0)
        label = (f'{vehicle["class"]} {kinematics["speed_kph_true"]:.0f}km/h'
                 f'{"" if visible >= 0.999 else f" {visible*100:.0f}%vis"}')
        draw.text((box["x"] + 3, max(2.0, box["y"] - 12)), label, fill=colour)

        obj["_contrast"] = target_contrast(image, box)

    scene = row["scene"]
    draw.text((10, 10),
              f'frame {row["frame_id"]}  {scene["weather"]}  '
              f'{len(row.get("objects", []))} object(s)', fill=(255, 255, 255))

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
    objects = [o for r in rows for o in r.get("objects", [])]
    per_frame = [len(r.get("objects", [])) for r in rows]

    scenes = Counter(r["scene"].get("weather", "unspecified") for r in rows)
    classes = Counter(o["vehicle"]["class"] for o in objects)
    valid = sum(1 for o in objects if o["geometry"]["bbox_valid"])
    clipped = sum(1 for o in objects if o["geometry"]["bbox_clipped"])
    speeds = [o["kinematics"]["speed_kph_true"] for o in objects]
    triggers = sum(1 for o in objects if o.get("triggered_capture"))

    print(f"frames         : {len(rows)}")
    print(f"objects        : {len(objects)}  ({min(per_frame)}-{max(per_frame)} per frame, "
          f"{len(objects)/max(len(rows),1):.1f} mean)")
    print(f"triggers       : {triggers}")
    print(f"bbox valid     : {valid}/{len(objects)}")
    print(f"bbox clipped   : {clipped}")
    if speeds:
        print(f"speed range    : {min(speeds):.1f} - {max(speeds):.1f} km/h")
    print(f"class balance  : {dict(classes)}")
    print(f"weather        : {dict(scenes)}")

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

    # Occlusion is the reason multi-vehicle traffic exists. If this stays at zero the
    # vehicles are not actually getting in each other's way.
    occluded = [o for o in objects if o.get("visibility", {}).get("occluded")]
    fractions = [o["visibility"]["visible_fraction"] for o in objects if "visibility" in o]
    if fractions:
        print(f"occluded       : {len(occluded)}/{len(objects)} objects "
              f"(visible fraction {min(fractions):.2f} to {max(fractions):.2f})")

    sizes = [o["geometry"]["bbox_xywh"]["w"] * o["geometry"]["bbox_xywh"]["h"] /
             (r["image_width"] * r["image_height"])
             for r in rows for o in r.get("objects", []) if o["geometry"].get("bbox_xywh")]
    if sizes:
        print(f"target size    : {min(sizes)*100:.1f}% to {max(sizes)*100:.1f}% of frame")

    # Contrast now comes from the engine, measured on the frame it rendered. The
    # locally recomputed value is kept only as a cross-check that the two agree.
    engine = [o["visibility"]["contrast"] for o in objects
              if "contrast" in o.get("visibility", {})]
    if engine:
        faint = [o for o in objects
                 if o.get("visibility", {}).get("contrast", 99) < LOW_CONTRAST]
        print(f"target contrast: {min(engine):.1f} to {max(engine):.1f}  (from labels)")
        if faint:
            print(f"                 {len(faint)} object(s) below {LOW_CONTRAST} - barely "
                  f"separable; filter on visibility.contrast when training")

    local = [o["_contrast"] for o in objects if "_contrast" in o]
    if local and engine and len(local) == len(engine):
        drift = max(abs(a - b) for a, b in zip(sorted(local), sorted(engine)))
        print(f"contrast agree : within {drift:.1f} of the engine's own measurement")

    print(f"previews       : {preview_dir}")
    for message in problems:
        print(f"PROBLEM        : {message}")


main()
