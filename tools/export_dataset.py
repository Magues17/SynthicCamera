"""
Export a capture run into the shared folder layout.

    python tools/export_dataset.py Saved/SynthData/<run> [--out exported] [--format all]

Produces, alongside each other so a frame and its annotation are always findable
from one another by name:

    exported/images/frame_000000.png
    exported/yaml/frame_000000.yaml
    exported/json/frame_000000.json      (--format json|all)
    exported/annotations.csv             (--format csv|all)

The camera writes PNG plus one JSONL line per pass; this turns that into per-frame
files. Kept as a separate step rather than written by the engine because the run is
the expensive part - re-exporting in another layout should not mean re-rendering.
"""

import argparse
import csv
import json
import shutil
from pathlib import Path

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
    parser.add_argument("--format", default="all", choices=["yaml", "json", "csv", "all"])
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

    images = args.out / "images"
    images.mkdir(parents=True, exist_ok=True)
    for name, wanted in (("yaml", want_yaml), ("json", want_json)):
        if wanted:
            (args.out / name).mkdir(parents=True, exist_ok=True)

    missing = []
    flat_rows = []

    for row in rows:
        stem = f"frame_{row['frame_id']:06d}"
        source = args.run / row["image_file"]

        if source.exists():
            target = images / f"{stem}.png"
            if args.move:
                shutil.move(str(source), target)
            else:
                shutil.copy2(source, target)
            # Point the annotation at the exported name, not the run's internal one.
            row = dict(row, image_file=f"images/{stem}.png")
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

    print(f"exported {len(rows)} frames to {args.out}")
    print(f"  images/     {len(rows) - len(missing)}")
    if want_yaml:
        print(f"  yaml/       {len(rows)}")
    if want_json:
        print(f"  json/       {len(rows)}")
    if want_csv:
        print(f"  annotations.csv  {len(flat_rows)} rows x {len(columns)} columns")

    objects = [o for r in rows for o in r.get("objects", [])]
    occluded = sum(1 for o in objects if o.get("visibility", {}).get("occluded"))
    unseen = sum(1 for o in objects if not o.get("visibility", {}).get("camera_can_see", True))
    print(f"  objects     {len(objects)} across {len(rows)} frames")
    print(f"  occluded    {occluded}")
    print(f"  not visible {unseen}")

    for name in missing:
        print(f"  MISSING IMAGE  {name}")


main()
