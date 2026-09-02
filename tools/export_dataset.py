"""
Export a capture run into the shared folder layout.

    python tools/export_dataset.py Saved/SynthData/<run> [--out exported] [--format all]

Produces, alongside each other so a frame and its annotation are always findable
from one another by name:

    exported/images/frame_000000.png
    exported/labels/frame_000000.txt     YOLO: class cx cy w h, normalised
    exported/data.yaml                   YOLO class map
    exported/classes.txt
    exported/annotations_coco.json       COCO instances
    exported/yaml/frame_000000.yaml      full label, every field
    exported/json/frame_000000.json
    exported/annotations.csv             one row per object

YOLO and COCO are here because the full label is not loadable by any trainer as-is.
They carry the boxes; the yaml/json keep everything the pipeline knows - speed, radar
reading, cosine angle, 3D cuboid corners, scene conditions - which no detection format
has a place for.

The camera writes PNG plus one JSONL line per pass; this turns that into per-frame
files. Kept as a separate step rather than written by the engine because the run is
the expensive part - re-exporting in another layout should not mean re-rendering.
"""

import argparse
import csv
import json
import shutil
from pathlib import Path

# Fixed order so a class always maps to the same id. Deriving ids from whatever
# classes happen to appear in a run would renumber everything the first time a run
# contains no tanks, silently relabelling an entire dataset.
CLASS_ORDER = [
    "LightUtility", "CargoTruck", "APC", "IFV", "MBT",
    "Car", "SUV", "Pickup", "BoxTruck",
]
CLASS_ID = {name: index for index, name in enumerate(CLASS_ORDER)}

NEWLINE = chr(10)

# Nesting depth is bounded and the schema is known, so a tiny emitter beats adding a
# PyYAML dependency for one output format.
def to_yaml(value, indent=0):
    pad = "  " * indent
    if isinstance(value, dict):
        if not value:
            return pad + "{}"
        lines = []
        for key, item in value.items():
            if isinstance(item, (dict, list)) and item:
                lines.append(f"{pad}{key}:")
                lines.append(to_yaml(item, indent + 1))
            else:
                lines.append(f"{pad}{key}: {scalar(item)}")
        return "\n".join(lines)

    if isinstance(value, list):
        if not value:
            return pad + "[]"
        lines = []
        for item in value:
            if isinstance(item, dict):
                block = to_yaml(item, indent + 1).lstrip()
                lines.append(f"{pad}- {block}")
            else:
                lines.append(f"{pad}- {scalar(item)}")
        return "\n".join(lines)

    return pad + scalar(value)


def scalar(value):
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, float):
        return f"{value:.6g}"
    if isinstance(value, (int,)):
        return str(value)
    text = str(value)
    # Quote anything YAML would otherwise reinterpret as a number, bool or null.
    if text == "" or text.strip() != text or text.lower() in {"true", "false", "null", "yes", "no"}:
        return f'"{text}"'
    try:
        float(text)
        return f'"{text}"'
    except ValueError:
        return text


def write_yolo(rows, out_dir, min_contrast, min_visible):
    """One text file per image: class_id cx cy w h, all normalised 0-1.

    YOLO has no way to mark an annotation as ignore, so anything not worth training
    on has to be dropped here rather than flagged. What gets dropped is reported,
    because a filter that silently removes a third of the data is indistinguishable
    from a bug.
    """
    labels = out_dir / "labels"
    labels.mkdir(parents=True, exist_ok=True)

    written = dropped = 0
    for row in rows:
        width, height = row["image_width"], row["image_height"]
        lines = []

        for obj in row.get("objects", []):
            box = obj["geometry"].get("bbox_xywh")
            if not box:
                continue

            visibility = obj.get("visibility", {})
            if visibility.get("contrast", 99.0) < min_contrast:
                dropped += 1
                continue
            if visibility.get("visible_fraction", 1.0) < min_visible:
                dropped += 1
                continue

            class_id = CLASS_ID.get(obj["vehicle"]["class"])
            if class_id is None:
                dropped += 1
                continue

            # YOLO wants centre and size as fractions of the image.
            cx = (box["x"] + box["w"] / 2) / width
            cy = (box["y"] + box["h"] / 2) / height
            lines.append(f'{class_id} {cx:.6f} {cy:.6f} '
                         f'{box["w"] / width:.6f} {box["h"] / height:.6f}')
            written += 1

        stem = f"frame_{row['frame_id']:06d}"
        # Written even when empty: YOLO treats a missing file as unlabelled rather
        # than as a background image, which is not the same thing.
        body = NEWLINE.join(lines)
        if lines:
            body += NEWLINE
        (labels / f"{stem}.txt").write_text(body, encoding="utf-8")

    (out_dir / "classes.txt").write_text(
        NEWLINE.join(CLASS_ORDER) + NEWLINE, encoding="utf-8")

    names = "".join(f"  {i}: {n}{NEWLINE}" for i, n in enumerate(CLASS_ORDER))
    (out_dir / "data.yaml").write_text(
        NEWLINE.join(["path: .", "train: images", "val: images", "", "names:", ""]) + names,
        encoding="utf-8")

    return written, dropped


def write_coco(rows, out_dir):
    """A single COCO instances file. Boxes stay absolute [x, y, w, h] as COCO expects.

    Occlusion, contrast and the military flag ride along as extra keys - COCO loaders
    ignore what they do not recognise, so nothing is lost by keeping them.
    """
    images, annotations = [], []
    annotation_id = 1

    for row in rows:
        image_id = row["frame_id"] + 1
        images.append({
            "id": image_id,
            "file_name": row["image_file"],
            "width": row["image_width"],
            "height": row["image_height"],
        })

        for obj in row.get("objects", []):
            box = obj["geometry"].get("bbox_xywh")
            class_id = CLASS_ID.get(obj["vehicle"]["class"])
            if not box or class_id is None:
                continue

            visibility = obj.get("visibility", {})
            annotations.append({
                "id": annotation_id,
                "image_id": image_id,
                "category_id": class_id + 1,          # COCO ids are 1-based
                "bbox": [round(box["x"], 2), round(box["y"], 2),
                         round(box["w"], 2), round(box["h"], 2)],
                "area": round(box["w"] * box["h"], 2),
                "iscrowd": 0,
                "synthic": {
                    "military": obj["vehicle"]["military"],
                    "model": obj["vehicle"]["model"],
                    "speed_kph_true": obj["kinematics"]["speed_kph_true"],
                    "speed_kph_radar": obj["kinematics"]["speed_kph_radar"],
                    "visible_fraction": visibility.get("visible_fraction"),
                    "occluded": visibility.get("occluded"),
                    "truncated": visibility.get("truncated"),
                    "contrast": visibility.get("contrast"),
                    "triggered_capture": obj.get("triggered_capture", False),
                },
            })
            annotation_id += 1

    document = {
        "info": {"description": "Synthic Camera synthetic speed-camera dataset"},
        "images": images,
        "annotations": annotations,
        "categories": [{"id": i + 1, "name": n, "supercategory": "vehicle"}
                       for i, n in enumerate(CLASS_ORDER)],
    }
    (out_dir / "annotations_coco.json").write_text(
        json.dumps(document, indent=1), encoding="utf-8")
    return len(annotations)


def flatten(prefix, value, out):
    """Flatten nested label fields into dotted CSV columns."""
    if isinstance(value, dict):
        for key, item in value.items():
            flatten(f"{prefix}.{key}" if prefix else key, item, out)
    elif isinstance(value, list):
        # Vertex lists become v0.x, v0.y, v1.x ... so a row stays one flat record.
        for index, item in enumerate(value):
            flatten(f"{prefix}.v{index}", item, out)
    else:
        out[prefix] = value


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("run", type=Path, help="a Saved/SynthData/<run> directory")
    parser.add_argument("--out", type=Path, default=Path("exported"))
    parser.add_argument("--format", default="all",
                        choices=["yaml", "json", "csv", "yolo", "coco", "all"])
    parser.add_argument("--min-contrast", type=float, default=6.0,
                        help="YOLO only: drop objects less separable than this from "
                             "their background (0 keeps everything)")
    parser.add_argument("--min-visible", type=float, default=0.15,
                        help="YOLO only: drop objects with less than this fraction of "
                             "their bounds unobstructed")
    parser.add_argument("--move", action="store_true",
                        help="move images instead of copying (saves disk on large runs)")
    args = parser.parse_args()

    labels = args.run / "labels.jsonl"
    if not labels.exists():
        raise SystemExit(f"no labels.jsonl in {args.run}")

    rows = []
    for number, line in enumerate(labels.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError as error:
            # A truncated final line means the run was killed mid-write. Say so rather
            # than silently exporting a short dataset.
            raise SystemExit(f"{labels}:{number} is not valid JSON: {error}")

    want_yaml = args.format in ("yaml", "all")
    want_json = args.format in ("json", "all")
    want_csv = args.format in ("csv", "all")
    want_yolo = args.format in ("yolo", "all")
    want_coco = args.format in ("coco", "all")

    images = args.out / "images"
    images.mkdir(parents=True, exist_ok=True)
    for name, wanted in (("yaml", want_yaml), ("json", want_json)):
        if wanted:
            (args.out / name).mkdir(parents=True, exist_ok=True)

    missing = []
    flat_rows = []

    for index, row in enumerate(rows):
        stem = f"frame_{row['frame_id']:06d}"
        source = args.run / row["image_file"]

        if source.exists():
            target = images / f"{stem}.png"
            if args.move:
                shutil.move(str(source), target)
            else:
                shutil.copy2(source, target)
            # Point the annotation at the exported name, not the run's internal one,
            # and write it back into the list. Rebinding the loop variable alone left
            # `rows` holding the run's names, so anything reading the list afterwards -
            # the COCO writer did - emitted paths that resolve to nothing.
            row = dict(row, image_file=f"images/{stem}.png")
            rows[index] = row
        else:
            missing.append(row["image_file"])

        if want_yaml:
            (args.out / "yaml" / f"{stem}.yaml").write_text(
                to_yaml(row) + "\n", encoding="utf-8")
        if want_json:
            (args.out / "json" / f"{stem}.json").write_text(
                json.dumps(row, indent=2) + "\n", encoding="utf-8")
        if want_csv:
            # One CSV row per object, with the frame's fields repeated. A frame with
            # three vehicles cannot be one row without inventing numbered columns that
            # no loader would know how to read back.
            frame_fields = {k: v for k, v in row.items() if k != "objects"}
            for obj in row.get("objects", []) or [None]:
                flat = {}
                flatten("", frame_fields, flat)
                if obj is not None:
                    flatten("", obj, flat)
                flat_rows.append(flat)

    if want_csv and flat_rows:
        columns = list(dict.fromkeys(key for row in flat_rows for key in row))
        with open(args.out / "annotations.csv", "w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=columns)
            writer.writeheader()
            writer.writerows(flat_rows)

    # Written after the image copy loop, so image_file already points at the exported
    # name rather than the run's internal one.
    yolo_written = yolo_dropped = coco_written = 0
    if want_yolo:
        yolo_written, yolo_dropped = write_yolo(rows, args.out,
                                                args.min_contrast, args.min_visible)
    if want_coco:
        coco_written = write_coco(rows, args.out)

    print(f"exported {len(rows)} frames to {args.out}")
    print(f"  images/     {len(rows) - len(missing)}")
    if want_yaml:
        print(f"  yaml/       {len(rows)}")
    if want_json:
        print(f"  json/       {len(rows)}")
    if want_csv:
        print(f"  annotations.csv  {len(flat_rows)} rows x {len(columns)} columns")
    if want_yolo:
        print(f"  labels/     {yolo_written} boxes  ({yolo_dropped} dropped below "
              f"contrast {args.min_contrast} / visibility {args.min_visible})")
        print(f"  data.yaml   {len(CLASS_ORDER)} classes")
    if want_coco:
        print(f"  annotations_coco.json  {coco_written} annotations")

    objects = [o for r in rows for o in r.get("objects", [])]
    occluded = sum(1 for o in objects if o.get("visibility", {}).get("occluded"))
    unseen = sum(1 for o in objects if not o.get("visibility", {}).get("camera_can_see", True))
    print(f"  objects     {len(objects)} across {len(rows)} frames")
    print(f"  occluded    {occluded}")
    print(f"  not visible {unseen}")

    for name in missing:
        print(f"  MISSING IMAGE  {name}")


main()
