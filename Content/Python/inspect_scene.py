"""
Dump every actor in Lvl_Desert to Saved/inspect_out.txt.

Loads the level explicitly. A headless commandlet opens no map by default, so any
inspection that relies on "the current level" reports an empty world and looks like
the scene failed to build when it did not.
"""

import os

import unreal

LEVEL_PATH = "/Game/Synthic/Lvl_Desert"

unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(LEVEL_PATH)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()

by_class = {}
for actor in actors:
    by_class.setdefault(actor.get_class().get_name(), []).append(
        (actor.get_actor_label(), actor.get_actor_location()))

lines = [f"level={LEVEL_PATH}", f"actor_count={len(actors)}", ""]
for class_name, entries in sorted(by_class.items(), key=lambda kv: -len(kv[1])):
    lines.append(f"{class_name}  x{len(entries)}")
    for label, location in sorted(entries):
        lines.append(f"  - {label:22s} ({location.x:9.0f},{location.y:9.0f},{location.z:7.0f})")

out_path = os.path.join(unreal.SystemLibrary.get_project_directory(), "Saved", "inspect_out.txt")
with open(out_path, "w", encoding="utf-8") as handle:
    handle.write("\n".join(lines))
unreal.log(f"[inspect_scene] {len(actors)} actors -> {out_path}")
