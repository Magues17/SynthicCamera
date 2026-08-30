"""
Generate a desert heightfield as a static mesh asset.

    powershell -File <skill>/scripts/run_ue_python.ps1 ^
        -Project SynthicCamera.uproject -Script Content/Python/build_terrain.py

Writes /Game/Synthic/SM_DesertTerrain. build_desert.py places it; this builds only
the asset, so relief can be re-tuned without rebuilding the whole scene.

A generated heightfield rather than scattered shapes: a heightfield is one continuous
surface, so it has no intersection seams. Overlapping ellipsoids were tried first and
cannot work - two closed surfaces clipping through each other leave hard straight
edges wherever they meet, and no amount of tuning removes them.

Two dampings shape it:

  Corridor  Height eases to zero across the road. Vehicles are kinematic and drive at
            a fixed height with no ground-following, so relief under the carriageway
            would leave them hovering or buried. The result is a graded cutting
            blending into rising ground, which is what a desert highway looks like.

  Border    Height eases to zero at the sheet edge, so the mesh meets the large flat
            ground plane underneath it without a visible lip. That is what lets this
            cover only the near field instead of the full 12km.
"""

import math
import os
import random

import unreal

ASSET_PATH = "/Game/Synthic"
ASSET_NAME = "SM_DesertTerrain"

EXTENT_CM = 250000.0        # half-width; 5km square, well past anything in frame
RESOLUTION = 200            # vertices per side -> 25m between samples

CORRIDOR_FLAT_CM = 1800.0   # dead flat out to here: road, shoulders, verge
CORRIDOR_BLEND_CM = 17000.0 # full relief reached by here

RELIEF_CM = 4200.0          # peak-to-trough of the broadest dunes
SEED = 20260830


def fade(t):
    """Perlin's quintic ease - smooth first and second derivatives, so lit terrain
    shows no faceted banding along the interpolation grid."""
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0)


def smoothstep(t):
    return t * t * (3.0 - 2.0 * t)


def build_gradients(size, rng):
    return [[(math.cos(a), math.sin(a))
             for a in (rng.uniform(0.0, math.tau) for _ in range(size + 1))]
            for _ in range(size + 1)]


def perlin(x, y, gradients, size):
    """Roughly [-1, 1]. Hand-rolled: UE's noise nodes live in the material graph and
    are not reachable from mesh generation."""
    xi, yi = int(x) % size, int(y) % size
    xf, yf = x - math.floor(x), y - math.floor(y)
    u, v = fade(xf), fade(yf)

    def dot(gx, gy, dx, dy):
        g = gradients[gy][gx]
        return g[0] * dx + g[1] * dy

    n00 = dot(xi, yi, xf, yf)
    n10 = dot(xi + 1, yi, xf - 1.0, yf)
    n01 = dot(xi, yi + 1, xf, yf - 1.0)
    n11 = dot(xi + 1, yi + 1, xf - 1.0, yf - 1.0)
    return (n00 * (1 - u) + n10 * u) * (1 - v) + (n01 * (1 - u) + n11 * u) * v


def fbm(x, y, gradients, size):
    """Layered noise: broad dunes with progressively finer detail on top."""
    total, frequency, amplitude, normaliser = 0.0, 1.0, 1.0, 0.0
    for _ in range(5):
        total += perlin(x * frequency, y * frequency, gradients, size) * amplitude
        normaliser += amplitude
        frequency *= 2.07       # not exactly 2, or the octaves align into visible grids
        amplitude *= 0.5
    return total / normaliser


def corridor_weight(y):
    """0 across the road, easing to 1 once clear. Smoothstepped so the corridor leaves
    flat gently - a linear ramp leaves a crease running the length of the road."""
    distance = abs(y)
    if distance <= CORRIDOR_FLAT_CM:
        return 0.0
    if distance >= CORRIDOR_BLEND_CM:
        return 1.0
    return smoothstep((distance - CORRIDOR_FLAT_CM) /
                      (CORRIDOR_BLEND_CM - CORRIDOR_FLAT_CM))


def border_weight(x, y):
    """Ease to 0 at the sheet edge so the mesh sinks into the flat ground plane."""
    margin = EXTENT_CM * 0.22
    edge = min(EXTENT_CM - abs(x), EXTENT_CM - abs(y))
    if edge >= margin:
        return 1.0
    return smoothstep(max(edge, 0.0) / margin)


def main():
    rng = random.Random(SEED)
    grid = 64
    gradients = build_gradients(grid, rng)

    step = (EXTENT_CM * 2.0) / (RESOLUTION - 1)
    noise_scale = 6.0 / (RESOLUTION - 1)

    heights = []
    for row in range(RESOLUTION):
        y = -EXTENT_CM + row * step
        line = []
        for col in range(RESOLUTION):
            x = -EXTENT_CM + col * step
            h = fbm(col * noise_scale, row * noise_scale, gradients, grid)
            line.append(h * RELIEF_CM * corridor_weight(y) * border_weight(x, y))
        heights.append(line)

    description = unreal.StaticMesh.create_static_mesh_description()

    vertices = []
    for row in range(RESOLUTION):
        y = -EXTENT_CM + row * step
        for col in range(RESOLUTION):
            x = -EXTENT_CM + col * step
            vertex = description.create_vertex()
            description.set_vertex_position(vertex, unreal.Vector(x, y, heights[row][col]))
            vertices.append(vertex)

    group = description.create_polygon_group()

    # Name the slot, or the built mesh has no material slots at all and assigning a
    # material by index later silently does nothing - the terrain then renders in the
    # engine's default grey, which reads as a lighting fault rather than a missing
    # binding.
    description.set_polygon_group_material_slot_name(group, "Sand")

    # UVs in metres, not normalised across the sheet: one 0-1 span over 5km stretches
    # any surface detail into invisibility.
    uv = step / 100.0

    def corner(row, col):
        instance = description.create_vertex_instance(vertices[row * RESOLUTION + col])
        description.set_vertex_instance_uv(instance, unreal.Vector2D(col * uv, row * uv), 0)
        return instance

    # Winding matters and is easy to get backwards. With x increasing along columns
    # and y along rows, ordering a quad (a, row+1, row+1/col+1) puts the right-hand
    # normal along -Z: the surface then faces down, and every frame showed terrain
    # lit from underneath - dark olive against pale sand, which reads as a material
    # or lighting fault rather than as geometry pointing the wrong way.
    for row in range(RESOLUTION - 1):
        for col in range(RESOLUTION - 1):
            description.create_triangle(group, [corner(row, col),
                                                corner(row + 1, col + 1),
                                                corner(row + 1, col)])
            description.create_triangle(group, [corner(row, col),
                                                corner(row, col + 1),
                                                corner(row + 1, col + 1)])

    full_path = ASSET_PATH + "/" + ASSET_NAME

    # Reuse the asset if it exists rather than deleting and recreating it. Once the
    # level references the terrain, the delete fails silently and create_asset then
    # returns None - the same trap the materials hit, where a rebuild worked once and
    # broke on every run afterwards.
    if unreal.EditorAssetLibrary.does_asset_exist(full_path):
        mesh = unreal.EditorAssetLibrary.load_asset(full_path)
    else:
        # No factory: UE has no StaticMeshFactoryNew, and create_asset accepts None
        # for asset types needing no import settings. Geometry comes from the mesh
        # description below, not from a factory.
        mesh = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            ASSET_NAME, ASSET_PATH, unreal.StaticMesh, None)

    if mesh is None:
        raise RuntimeError("could not create or load " + full_path)
    # fast_build=False. With it on, the build skips normal and tangent generation, and
    # the mesh description API exposes no way to set normals from Python - so the
    # terrain shaded almost black against the plane beside it. Slower, and correct.
    mesh.build_from_static_mesh_descriptions([description], False, False)

    subsystem = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    try:
        options = subsystem.get_lod_build_settings(mesh, 0)
        options.set_editor_property("recompute_normals", True)
        options.set_editor_property("recompute_tangents", True)
        options.set_editor_property("use_full_precision_u_vs", True)
        subsystem.set_lod_build_settings(mesh, 0, options)
    except Exception as error:
        unreal.log_warning("[build_terrain] could not force normal recompute: %s" % error)
    unreal.EditorAssetLibrary.save_asset(full_path)

    peak = max(max(abs(v) for v in line) for line in heights)
    out = os.path.join(unreal.SystemLibrary.get_project_directory(), "Saved",
                       "terrain_out.txt")
    with open(out, "w", encoding="utf-8") as handle:
        handle.write(chr(10).join([
            "asset=" + full_path,
            "vertices=%d" % len(vertices),
            "triangles=%d" % ((RESOLUTION - 1) * (RESOLUTION - 1) * 2),
            "sample_spacing_cm=%.0f" % step,
            "peak_height_cm=%.0f" % peak,
            "corridor_flat_cm=%.0f" % CORRIDOR_FLAT_CM,
        ]))

    unreal.log("[build_terrain] wrote " + full_path)


main()
