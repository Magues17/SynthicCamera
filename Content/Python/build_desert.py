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
DIRECTOR_POSITION = unreal.Vector(-12000.0, 0.0, 0.0)

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


def build_ground(sand, rock):
    lh.spawn_block("Desert_Ground", 0, 0, -100,
                   GROUND_SIZE_CM, GROUND_SIZE_CM, 200, material=sand)

    # Background relief so the horizon is not an empty plane. Seeded: the scene must
    # regenerate identically or the dataset is not reproducible.
    stream = unreal.RandomStream()
    stream.initialize(20260829)
    for index in range(24):
        x = stream.rand_range(-30000, 30000)
        y = stream.rand_range(-30000, 30000)
        if abs(y) < ROAD_WIDTH_CM * 2:
            continue                                    # keep the carriageway clear
        size = stream.rand_range(300, 1400)
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

    aim = unreal.MathLibrary.find_look_at_rotation(CAMERA_POSITION, CAMERA_AIM_POINT)

    capture = camera.get_editor_property("CaptureComponent")
    capture.set_editor_property("relative_rotation", aim)

    # Trigger sits over the carriageway at the aim point, not at the pole, so the
    # vehicle is near frame centre at the instant the shutter fires.
    trigger = camera.get_editor_property("TriggerVolume")
    trigger.set_editor_property("relative_location", unreal.Vector(
        CAMERA_AIM_POINT.x - CAMERA_POSITION.x,
        CAMERA_AIM_POINT.y - CAMERA_POSITION.y,
        CAMERA_AIM_POINT.z - CAMERA_POSITION.z))
    trigger.set_editor_property("box_extent", unreal.Vector(60.0, ROAD_WIDTH_CM * 0.5, 400.0))

    lh.log(f"camera at {CAMERA_POSITION} aiming {aim}")
    return camera


def build_director():
    """Start line at the far end, forward vector (+X) pointing down the road."""
    director = spawn_synth_actor("/Script/SynthicCamera.SynthCaptureDirector",
                                 DIRECTOR_POSITION, unreal.Rotator(0, 0, 0))
    director.set_actor_label("CaptureDirector")
    return director


def main():
    lh.fresh_level_from_template(LEVEL_PATH)
    lh.prune_props()

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
