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

import os
import random
import sys

import unreal

SKILL_SCRIPTS = r"C:\Users\mikel\.claude\skills\unreal-editor-automation\scripts"
if SKILL_SCRIPTS not in sys.path:
    sys.path.append(SKILL_SCRIPTS)

import level_helpers as lh

LEVEL_PATH = "/Game/Synthic/Lvl_Desert"

# --- Scene geometry -------------------------------------------------------------
# The ground has to outrun visibility or its far edge shows as a hard line against
# the sky - and the randomised camera roll tilts that line, which makes it read as a
# seam rather than a horizon. 800m failed, 3km still failed in clear weather; 12km
# puts the edge past where any of the fog presets can be seen through. The road
# follows suit: a carriageway that simply stops mid-desert is just as obvious.
GROUND_SIZE_CM = 1200000.0
ROAD_LENGTH_CM = 140000.0
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
    "num_passes": 30,
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

    # Camera install is re-sited per pass. Ranges live on the CameraPose struct and
    # are left at their C++ defaults; set randomise_camera_pose False to model one
    # specific real installation instead of a spread of them.
    "randomise_camera_pose": True,
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
AMBIENT_VOLUME_LABEL = "PPV_MatchCaptures"



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


def match_viewport_to_captures():
    """Give the editor and play views the same ambient fill the captures get.

    The ambient cubemap lives on the capture component's post-process settings, so
    without this the viewport renders the scene with a lighting model the dataset
    never sees - shadows read black by eye while the actual output is fine, which
    makes judging the scene visually worthless.
    """
    for actor in _editor_actor.get_all_level_actors():
        if actor.get_actor_label() == AMBIENT_VOLUME_LABEL:
            _editor_actor.destroy_actor(actor)

    volume = _editor_actor.spawn_actor_from_class(
        unreal.PostProcessVolume, unreal.Vector(0.0, 0.0, 0.0), unreal.Rotator(0, 0, 0))
    volume.set_actor_label(AMBIENT_VOLUME_LABEL)
    volume.set_editor_property("unbound", True)

    # /Engine/ content is not in the project asset registry until it is scanned, so
    # load_asset on an engine path fails outright without this.
    unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous(
        ["/Engine/MapTemplates/Sky"], True)

    cubemap = unreal.EditorAssetLibrary.load_asset(
        "/Engine/MapTemplates/Sky/DaylightAmbientCubemap")
    if cubemap is None:
        lh.log("WARNING ambient cubemap missing; viewport will not match captures")
        return

    settings = volume.get_editor_property("settings")
    settings.set_editor_property("ambient_cubemap", cubemap)
    settings.set_editor_property("override_ambient_cubemap_intensity", True)
    settings.set_editor_property("ambient_cubemap_intensity", CAMERA_SETTINGS["ambient_intensity"])
    volume.set_editor_property("settings", settings)
    lh.log("viewport ambient matched to captures")


def remove_volumetric_clouds():
    """Delete the template's volumetric cloud layer.

    A camera at road level looks along the horizon, where the cloud layer's flat base
    cuts the sky as a hard straight seam - and the randomised camera roll tilts it, so
    it reads as a polygon edge rather than weather. Physically it is a cloud base;
    visually it is the most artificial thing left in frame.

    The sky atmosphere still supplies colour and horizon gradient, so what is lost is
    cloud detail in the upper sky, which no capture is aimed at.
    """
    for actor in _editor_actor.get_all_level_actors():
        if actor.get_class().get_name() == "VolumetricCloud":
            _editor_actor.destroy_actor(actor)
            lh.log("removed volumetric cloud layer")


def remove_stray_directional_lights():
    """Delete every directional light except the template's own.

    An earlier version of this script spawned a fill light here, and when that
    approach was abandoned its cleanup went with it - leaving the light stranded in
    the saved level, where prune_props keeps it because it keeps all lighting. UE
    then picks a main light by brightness for fog, translucency and volumetrics,
    which quietly changes how the scene renders and is exactly the sort of thing
    that never shows up as an error.
    """
    removed = []
    for actor in _editor_actor.get_all_level_actors():
        if actor.get_class().get_name() != "DirectionalLight":
            continue
        if actor.get_actor_label() == "DirectionalLight":
            continue                                # the template's sun; keep it
        removed.append(actor.get_actor_label())
        _editor_actor.destroy_actor(actor)

    for label in removed:
        lh.log(f"removed stray directional light: {label}")
    return removed


def report_directional_lights():
    """Audit directional lights to Saved/lights_out.txt.

    More than one and UE picks a main light arbitrarily for fog, translucency and
    volumetrics, which silently changes how the scene renders. Written to a file
    because unreal.log does not reliably reach commandlet stdout.
    """
    lights = [a for a in _editor_actor.get_all_level_actors()
              if a.get_class().get_name() == "DirectionalLight"]

    lines = [f"directional_light_count={len(lights)}"]
    for light in lights:
        loc = light.get_actor_location()
        lines.append(f"  {light.get_actor_label()}  ({loc.x:.0f},{loc.y:.0f},{loc.z:.0f})")
    if len(lights) > 1:
        lines.append("WARNING more than one - UE will pick a main light by brightness")

    out = os.path.join(unreal.SystemLibrary.get_project_directory(), "Saved", "lights_out.txt")
    with open(out, "w", encoding="utf-8") as handle:
        handle.write(chr(10).join(lines))
    return lights


def make_surface_material(name, colour_dark, colour_light, noise_scale,
                          rough_min, rough_max, specular=0.3, folder="/Game/Materials"):
    """A ground surface with procedural variation rather than one flat colour.

    A single Constant3Vector into BaseColor is what makes untextured geometry read as
    modelling clay: no colour break-up and a uniform default roughness, so every face
    returns the same value and the eye gets no surface cue at all.

    Noise drives colour and roughness together here, which is what sells sand as sand.
    Deliberately procedural rather than texture-based - the texture packs are not
    committed, and a material that breaks on a fresh clone is worse than a plain one.
    """
    path = folder + "/" + name
    if not unreal.EditorAssetLibrary.does_directory_exist(folder):
        unreal.EditorAssetLibrary.make_directory(folder)

    editing = unreal.MaterialEditingLibrary

    # Rewire in place rather than delete and recreate. Once the level references a
    # material, deleting it fails silently and the create that follows then errors -
    # so a rebuild worked exactly once and broke on every run after.
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        material = unreal.EditorAssetLibrary.load_asset(path)
        editing.delete_all_material_expressions(material)
    else:
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, folder, unreal.Material, unreal.MaterialFactoryNew())

    def node(cls, x, y):
        return editing.create_material_expression(material, cls, x, y)

    noise = node(unreal.MaterialExpressionNoise, -900, 0)
    noise.set_editor_property("scale", noise_scale)
    noise.set_editor_property("levels", 4)
    noise.set_editor_property("output_min", 0.0)
    noise.set_editor_property("output_max", 1.0)
    noise.set_editor_property("turbulence", True)

    dark = node(unreal.MaterialExpressionConstant3Vector, -620, -180)
    dark.set_editor_property("constant", unreal.LinearColor(
        colour_dark[0], colour_dark[1], colour_dark[2], 1.0))
    light = node(unreal.MaterialExpressionConstant3Vector, -620, -60)
    light.set_editor_property("constant", unreal.LinearColor(
        colour_light[0], colour_light[1], colour_light[2], 1.0))

    colour = node(unreal.MaterialExpressionLinearInterpolate, -320, -120)
    editing.connect_material_expressions(dark, "", colour, "A")
    editing.connect_material_expressions(light, "", colour, "B")
    editing.connect_material_expressions(noise, "", colour, "Alpha")
    editing.connect_material_property(colour, "", unreal.MaterialProperty.MP_BASE_COLOR)

    low = node(unreal.MaterialExpressionConstant, -620, 120)
    low.set_editor_property("r", rough_min)
    high = node(unreal.MaterialExpressionConstant, -620, 200)
    high.set_editor_property("r", rough_max)

    roughness = node(unreal.MaterialExpressionLinearInterpolate, -320, 160)
    editing.connect_material_expressions(low, "", roughness, "A")
    editing.connect_material_expressions(high, "", roughness, "B")
    editing.connect_material_expressions(noise, "", roughness, "Alpha")
    editing.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)

    spec = node(unreal.MaterialExpressionConstant, -620, 300)
    spec.set_editor_property("r", specular)
    editing.connect_material_property(spec, "", unreal.MaterialProperty.MP_SPECULAR)

    editing.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(path)
    lh.log("built surface material " + path)
    return material


def build_ground(sand, rock):
    lh.spawn_block("Desert_Ground", 0, 0, -100,
                   GROUND_SIZE_CM, GROUND_SIZE_CM, 200, material=sand)

    # Background relief so the horizon is not an empty plane. Seeded: the scene must
    # regenerate identically or the dataset is not reproducible.
    #
    # Spheres crushed on one axis and tilted, not boxes. Axis-aligned cubes read as
    # masonry - they were mistaken for a wall the traffic drove through - and nothing
    # in a desert has four vertical faces and a flat top.
    sphere = unreal.load_asset("/Engine/BasicShapes/Sphere.Sphere")
    stream = random.Random(20260829)

    for index in range(90):
        x = stream.uniform(-160000, 160000)
        y = stream.uniform(-160000, 160000)
        if abs(y) < ROAD_WIDTH_CM * 3:
            continue                            # keep well clear of the carriageway

        size = stream.uniform(260, 1500)
        # Squashed and stretched independently so no two share a silhouette, and sunk
        # so they read as outcrops emerging from sand rather than balls resting on it.
        scale = unreal.Vector(size / 100.0,
                              size / 100.0 * stream.uniform(0.55, 1.05),
                              size / 100.0 * stream.uniform(0.30, 0.62))
        sink = size * stream.uniform(0.10, 0.26)

        actor = _editor_actor.spawn_actor_from_class(
            unreal.StaticMeshActor, unreal.Vector(x, y, -sink),
            unreal.Rotator(stream.uniform(-14, 14), stream.uniform(0, 360),
                           stream.uniform(-14, 14)))
        actor.set_actor_label("Rock_%02d" % index)
        actor.tags = ["level"]

        component = actor.static_mesh_component
        component.set_static_mesh(sphere)
        actor.set_actor_scale3d(scale)
        component.set_mobility(unreal.ComponentMobility.STATIC)
        try:
            component.set_material(0, rock)
        except Exception as error:
            lh.log("  warn rock material: %s" % error)
        component.set_collision_enabled(unreal.CollisionEnabled.QUERY_AND_PHYSICS)
        component.set_collision_profile_name("BlockAll")


def build_road(asphalt, line):
    lh.spawn_block("Road_Deck", 0, 0, ROAD_SURFACE_Z,
                   ROAD_LENGTH_CM, ROAD_WIDTH_CM, 20, material=asphalt)

    # Graded shoulders either side. Without them the tarmac is a slab dropped on sand
    # with a hard edge running to the horizon, which is the most artificial line in
    # the whole scene.
    shoulder = ROAD_WIDTH_CM * 0.5 + 130
    for name, y in (("Road_ShoulderL", -shoulder), ("Road_ShoulderR", shoulder)):
        lh.spawn_block(name, 0, y, ROAD_SURFACE_Z - 8,
                       ROAD_LENGTH_CM, 260, 16, material=asphalt, collision=False)

    edge = ROAD_WIDTH_CM * 0.5 - 25.0
    for name, y in (("Road_EdgeL", -edge), ("Road_EdgeR", edge)):
        lh.spawn_block(name, 0, y, ROAD_SURFACE_Z + 12,
                       ROAD_LENGTH_CM, 14, 6, material=line, collision=False)

    # Dashed centreline. Beyond looking right, the dashes give the fog presets
    # something regular to fall off against, and they are the reference a real
    # installation uses to verify speed from marking transit time.
    dash, gap = 300.0, 900.0
    count = int(ROAD_LENGTH_CM // (dash + gap))
    start = -ROAD_LENGTH_CM * 0.5 + gap
    for index in range(count):
        lh.spawn_block("Road_Dash_%02d" % index,
                       start + index * (dash + gap), 0, ROAD_SURFACE_Z + 12,
                       dash, 12, 6, material=line, collision=False)


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

    # Noise scales are in world units: coarse drift across the dunes, fine grain on
    # the tarmac. The roughness ranges keep every surface matte - the 0.5 default
    # gives sand an unearned sheen that reads as plastic.
    sand = make_surface_material("M_Sand", (0.52, 0.43, 0.28), (0.80, 0.70, 0.50),
                                 0.0012, 0.86, 0.97)
    asphalt = make_surface_material("M_Asphalt", (0.030, 0.030, 0.034),
                                    (0.085, 0.085, 0.092), 0.0180, 0.62, 0.84,
                                    specular=0.42)
    rock = make_surface_material("M_Rock", (0.20, 0.16, 0.12), (0.46, 0.39, 0.30),
                                 0.0060, 0.78, 0.94)
    line = make_surface_material("M_RoadLine", (0.62, 0.61, 0.56), (0.86, 0.85, 0.80),
                                 0.0300, 0.55, 0.75, specular=0.5)

    build_ground(sand, rock)
    build_road(asphalt, line)
    remove_volumetric_clouds()
    remove_stray_directional_lights()
    match_viewport_to_captures()
    build_camera()
    build_director()
    report_directional_lights()

    # Off to the side, so pressing Play does not drop the pawn into traffic.
    _editor_actor.spawn_actor_from_class(
        unreal.PlayerStart, unreal.Vector(1200.0, -1800.0, 200.0), unreal.Rotator(0, 0, 0))

    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()
    lh.log(f"desert scene built at {LEVEL_PATH}")


main()
