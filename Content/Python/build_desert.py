"""
Build the desert speed-camera scene from scratch.

Run headless:
    powershell -File <skill>/scripts/run_ue_python.ps1 \
        -Project C:/Users/mikel/SynthicCamera/SynthicCamera.uproject \
        -Script  C:/Users/mikel/SynthicCamera/Content/Python/build_desert.py

The scene is deliberately described in code rather than hand-placed: the camera
geometry (height, lateral offset, aim point) is the single biggest influence on
what the dataset looks like, so it needs to be a value you can change and re-run,
not something buried in a .umap you tweaked by eye six weeks ago.

Layout, all centimetres, traffic travels along +X:

    director (start line)                    camera pole
    X = -12000 ------------------------------> X = +1200, Y = -700, Z = 550
                        ^ trigger + aim point at X = -1500
"""

import random
import sys

import unreal

SKILL_SCRIPTS = r"C:\Users\mikel\.claude\skills\unreal-editor-automation\scripts"
if SKILL_SCRIPTS not in sys.path:
    sys.path.append(SKILL_SCRIPTS)

import level_helpers as lh

LEVEL_PATH = "/Game/Synthic/Lvl_Desert"

# --- Scene geometry -------------------------------------------------------------
GROUND_SIZE_CM = 80000.0
ROAD_LENGTH_CM = 40000.0
ROAD_WIDTH_CM = 800.0
ROAD_SURFACE_Z = 6.0            # road deck sits just proud of the sand

CAMERA_POSITION = unreal.Vector(1200.0, -700.0, 550.0)
CAMERA_AIM_POINT = unreal.Vector(-1500.0, 0.0, 150.0)   # mid-lane, roughly bonnet height
DIRECTOR_POSITION = unreal.Vector(-12000.0, 0.0, 16.0)   # on the road deck, not in it

# --- Run parameters -------------------------------------------------------------
CAMERA_SETTINGS = {
    "image_width": 1280,
    "image_height": 720,
    "field_of_view_deg": 50.0,
    # Ambient fill for shadowed surfaces. A SkyLight does not reach SceneCapture2D
    # renders, so the camera applies an ambient cubemap in its own post-process chain.
    "ambient_intensity": 4.0,
}

RUN_SETTINGS = {
    "num_passes": 20,
    "random_seed": 1337,
    "min_speed_kph": 40.0,
    "max_speed_kph": 110.0,
    # Start line to camera is 13200cm; stop just past it rather than driving on into
    # empty desert, which is pure wall-clock after the photo is already taken.
    "travel_distance_cm": 15000.0,
    "lane_jitter_cm": 150.0,

    # Domain randomisation. Sun elevation is negative-is-downward, so -80 is near
    # overhead and -12 is a low raking sun; the full yaw circle decides which faces
    # of the vehicle are lit at all. WeatherMix is left to the C++ default (an even
    # spread of all four conditions) because a TArray of enums is awkward from here.
    "randomise_scene": True,
    "sun_pitch_range_deg": unreal.Vector2D(-80.0, -12.0),
    "sun_yaw_range_deg": unreal.Vector2D(0.0, 360.0),
    "weather_jitter": 0.25,
}

SAND = (0.76, 0.65, 0.45)
ASPHALT = (0.06, 0.06, 0.07)
LINE_WHITE = (0.85, 0.85, 0.82)
ROCK = (0.42, 0.35, 0.28)

_editor_actor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def spawn_synth_actor(class_path, location, rotation=None):
    """Spawn one of our C++ actors by path, so a stale binding fails loudly."""
    actor_class = unreal.load_class(None, class_path)
    if actor_class is None:
        raise RuntimeError(
            f"{class_path} not found. Did SynthicCameraEditor compile? "
            "Check Binaries/Win64/UnrealEditor-SynthicCamera.dll exists.")
    return _editor_actor.spawn_actor_from_class(
        actor_class, location, rotation or unreal.Rotator(0, 0, 0))


VEHICLE_BODY_MATERIAL = "M_VehicleBody"



def make_vehicle_body_material(folder="/Game/Materials"):
    """Material with a *parameter* driving base colour, not a baked constant.

    The engine's BasicShapeMaterial exposes no colour parameter, so the per-vehicle
    dynamic instance had nothing to set and every proxy rendered the same. A livery
    field in the label that the pixels do not reflect is worse than no field at all.
    """
    path = f"{folder}/{VEHICLE_BODY_MATERIAL}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        return unreal.EditorAssetLibrary.load_asset(path)

    if not unreal.EditorAssetLibrary.does_directory_exist(folder):
        unreal.EditorAssetLibrary.make_directory(folder)

    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        VEHICLE_BODY_MATERIAL, folder, unreal.Material, unreal.MaterialFactoryNew())
    editing = unreal.MaterialEditingLibrary

    colour = editing.create_material_expression(
        material, unreal.MaterialExpressionVectorParameter, -400, 0)
    colour.set_editor_property("parameter_name", "Color")
    colour.set_editor_property("default_value", unreal.LinearColor(0.21, 0.22, 0.15, 1.0))
    editing.connect_material_property(colour, "", unreal.MaterialProperty.MP_BASE_COLOR)

    roughness = editing.create_material_expression(
        material, unreal.MaterialExpressionConstant, -400, 200)
    roughness.set_editor_property("r", 0.65)
    editing.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)

    editing.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(path)
    lh.log(f"created {path}")
    return material


def build_ground(sand, rock):
    lh.spawn_block("Desert_Ground", 0, 0, -100,
                   GROUND_SIZE_CM, GROUND_SIZE_CM, 200, material=sand)

    # Background relief so the horizon is not an empty plane. Seeded: the scene must
    # regenerate identically or the dataset is not reproducible.
    stream = random.Random(20260829)
    for index in range(24):
        x = stream.uniform(-30000, 30000)
        y = stream.uniform(-30000, 30000)
        if abs(y) < ROAD_WIDTH_CM * 2:
            continue                                    # keep the carriageway clear
        size = stream.uniform(300, 1400)
        lh.spawn_block(f"Rock_{index:02d}", x, y, size * 0.25,
                       size, size * 0.8, size * 0.5, material=rock)


def build_road(asphalt, line):
    lh.spawn_block("Road_Deck", 0, 0, ROAD_SURFACE_Z,
                   ROAD_LENGTH_CM, ROAD_WIDTH_CM, 20, material=asphalt)

    # ponytail: solid edge lines only. Dashed centre markings matter if you ever want
    # secondary speed verification from marking transit time - add that loop then.
    edge = ROAD_WIDTH_CM * 0.5 - 25.0
    for name, y in (("Road_EdgeL", -edge), ("Road_EdgeR", edge)):
        lh.spawn_block(name, 0, y, ROAD_SURFACE_Z + 12,
                       ROAD_LENGTH_CM, 14, 6, material=line, collision=False)


def build_camera():
    """Pole-mounted camera. The actor stays unrotated; only the lens aims, which keeps
    the trigger's relative offset expressible in plain world axes."""
    camera = spawn_synth_actor("/Script/SynthicCamera.SynthSpeedCamera", CAMERA_POSITION)
    camera.set_actor_label("SpeedCamera")

    for name, value in CAMERA_SETTINGS.items():
        camera.set_editor_property(name, value)

    aim = unreal.MathLibrary.find_look_at_rotation(CAMERA_POSITION, CAMERA_AIM_POINT)

    capture = camera.get_editor_property("capture_component")
    capture.set_editor_property("relative_rotation", aim)

    # The trip plane sits over the carriageway at the aim point, not at the pole, so the
    # vehicle is near frame centre when the shutter fires. The camera actor is left
    # unrotated so this offset stays expressible in plain world axes, and so the capture
    # point's forward vector (+X) is the direction of travel - which is the plane normal.
    capture_point = camera.get_editor_property("capture_point")
    capture_point.set_editor_property("relative_location", unreal.Vector(
        CAMERA_AIM_POINT.x - CAMERA_POSITION.x,
        CAMERA_AIM_POINT.y - CAMERA_POSITION.y,
        CAMERA_AIM_POINT.z - CAMERA_POSITION.z))

    lh.log(f"camera at {CAMERA_POSITION} aiming {aim}")
    return camera


def build_director():
    """Start line at the far end, forward vector (+X) pointing down the road.

    Run parameters are set here rather than left to the C++ defaults: a placed actor
    serialises whatever the defaults were the day it was spawned, so a level built
    last month silently ignores today's default and the two disagree with no warning.
    """
    director = spawn_synth_actor("/Script/SynthicCamera.SynthCaptureDirector",
                                 DIRECTOR_POSITION, unreal.Rotator(0, 0, 0))
    director.set_actor_label("CaptureDirector")

    for name, value in RUN_SETTINGS.items():
        director.set_editor_property(name, value)

    return director


def main():
    # DefaultEngine.ini points the startup map at this level, so on a rebuild the
    # commandlet already has it open and deleting it would fail. Load-and-prune is
    # both the fix and the simpler path: this level is not World Partition, so prune
    # genuinely removes actors rather than silently no-opping on streaming cells.
    if unreal.EditorAssetLibrary.does_asset_exist(LEVEL_PATH):
        lh.log(f"{LEVEL_PATH} exists - loading and pruning for rebuild")
        unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(LEVEL_PATH)
    else:
        lh.fresh_level_from_template(LEVEL_PATH)

    lh.prune_props()

    make_vehicle_body_material()

    sand = lh.make_color_material("M_Sand", SAND)
    asphalt = lh.make_color_material("M_Asphalt", ASPHALT)
    line = lh.make_color_material("M_RoadLine", LINE_WHITE)
    rock = lh.make_color_material("M_Rock", ROCK)

    build_ground(sand, rock)
    build_road(asphalt, line)
    build_camera()
    build_director()

    # Off to the side, so pressing Play does not drop the pawn into traffic.
    _editor_actor.spawn_actor_from_class(
        unreal.PlayerStart, unreal.Vector(1200.0, -1800.0, 200.0), unreal.Rotator(0, 0, 0))

    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()
    lh.log(f"desert scene built at {LEVEL_PATH}")


main()
