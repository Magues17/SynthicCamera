"""
Measure every actor's real world-space bounds against the road corridor.

Reads the saved level rather than recomputing from the build script, because the
two can diverge - the script only says what a rebuild *would* produce, not what is
actually in the map that just ran.

Writes Saved/clearance_out.txt.
"""

import os

import unreal

LEVEL_PATH = "/Game/Synthic/Lvl_Desert"
ROAD_HALF_WIDTH_CM = 400.0      # build_desert.py ROAD_WIDTH_CM / 2

unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(LEVEL_PATH)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()

rows = []
for actor in actors:
    label = actor.get_actor_label()
    if label in ("Desert_Ground", "Road_Deck", "Road_EdgeL", "Road_EdgeR"):
        continue

    try:
        origin, extent = actor.get_actor_bounds(only_colliding_components=False)
    except Exception:
        continue

    if extent.x <= 1.0 and extent.y <= 1.0 and extent.z <= 1.0:
        continue                                    # lights, markers, empties

    # Closest approach of this actor's footprint to the road centreline, then to the
    # road edge. Negative means it is over the carriageway.
    near_edge = abs(origin.y) - extent.y
    clearance = near_edge - ROAD_HALF_WIDTH_CM

    rows.append((clearance, label, origin, extent))

rows.sort(key=lambda r: r[0])

lines = ["road_half_width_cm=%.0f" % ROAD_HALF_WIDTH_CM,
         "actors_measured=%d" % len(rows),
         "",
         "clearance_cm  label                  centre(x,y,z)            size(l,w,h)"]

for clearance, label, origin, extent in rows:
    flag = "  <-- OVER THE ROAD" if clearance < 0 else ""
    lines.append("%12.0f  %-22s (%7.0f,%7.0f,%6.0f)  %6.0f x %6.0f x %6.0f%s"
                 % (clearance, label, origin.x, origin.y, origin.z,
                    extent.x * 2, extent.y * 2, extent.z * 2, flag))

over = sum(1 for r in rows if r[0] < 0)
lines.append("")
lines.append("actors_over_road=%d" % over)

out = os.path.join(unreal.SystemLibrary.get_project_directory(), "Saved", "clearance_out.txt")
with open(out, "w", encoding="utf-8") as handle:
    handle.write(chr(10).join(lines))

unreal.log("[inspect_road_clearance] wrote " + out)
