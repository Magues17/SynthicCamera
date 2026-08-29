"""Dump the actual light settings in Lvl_Desert to Saved/lighting_out.txt."""

import os

import unreal

unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level("/Game/Synthic/Lvl_Desert")
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()

lines = []
for actor in actors:
    class_name = actor.get_class().get_name()
    if class_name not in ("SkyLight", "DirectionalLight", "SkyAtmosphere", "ExponentialHeightFog"):
        continue
    lines.append(f"=== {class_name} ({actor.get_actor_label()}) rot={actor.get_actor_rotation()}")
    try:
        component = actor.get_editor_property("light_component")
    except Exception as error:
        lines.append(f"  no light_component: {error}")
        continue
    for prop in ("intensity", "mobility", "real_time_capture", "source_type",
                 "lower_hemisphere_is_black", "volumetric_scattering_intensity",
                 "affects_world", "visible", "cubemap_resolution"):
        try:
            lines.append(f"  {prop} = {component.get_editor_property(prop)}")
        except Exception as error:
            lines.append(f"  {prop} = <unavailable: {type(error).__name__}>")

out = os.path.join(unreal.SystemLibrary.get_project_directory(), "Saved", "lighting_out.txt")
with open(out, "w", encoding="utf-8") as handle:
    handle.write("\n".join(lines))
unreal.log(f"[inspect_lighting] wrote {out}")
