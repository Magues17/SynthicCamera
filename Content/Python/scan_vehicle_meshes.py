"""
Report every static mesh under a content folder, with real-world size and pivot.

Run after adding vehicle assets from Fab. Writes Saved/meshes_out.txt with each
mesh's path, bounding size in centimetres, and where its pivot sits - the three
things needed to map an asset onto a vehicle class and seat it on the road.

    powershell -File <skill>/scripts/run_ue_python.ps1 ^
        -Project SynthicCamera.uproject -Script Content/Python/scan_vehicle_meshes.py

Size tells you the class: a nine-metre mesh is a tank, a four-metre one is a car.
Pivot tells you the offset: assets authored with the pivot at the model centre sink
halfway into the road unless lifted, and the label's bounding box goes with them.
"""

import os

import unreal

SCAN_ROOTS = ["/Game"]
IGNORE_PREFIXES = ("/Game/Synthic", "/Game/Materials", "/Game/Developers", "/Game/Collections")

registry = unreal.AssetRegistryHelpers.get_asset_registry()
registry.scan_paths_synchronous(SCAN_ROOTS, True)

lines = []
count = 0

for root in SCAN_ROOTS:
    for asset in registry.get_assets_by_path(root, recursive=True):
        path = str(asset.package_name)
        if path.startswith(IGNORE_PREFIXES):
            continue
        if str(asset.asset_class_path.asset_name) != "StaticMesh":
            continue

        mesh = unreal.EditorAssetLibrary.load_asset(path)
        if mesh is None:
            lines.append(path + "  <could not load>")
            continue

        bounds = mesh.get_bounds()
        extent = bounds.box_extent          # half-size in the mesh's own space
        origin = bounds.origin

        # A pivot at ground contact leaves the bounds centre a half-height above it.
        # A pivot at the model centre leaves it near zero. ground_offset_z is what the
        # catalog must add to seat the mesh on the road.
        ground_offset = extent.z - origin.z
        pivot = "ground" if abs(origin.z - extent.z) < max(extent.z * 0.15, 1.0) else "centre"

        count += 1
        lines.append(path)
        lines.append("    size_cm   L=%.0f  W=%.0f  H=%.0f"
                     % (extent.x * 2, extent.y * 2, extent.z * 2))
        lines.append("    origin    (%.0f,%.0f,%.0f)  pivot=%s  ground_offset_z=%.0f"
                     % (origin.x, origin.y, origin.z, pivot, ground_offset))
        lines.append("    sections  %d" % mesh.get_num_sections(0))

header = ["static_mesh_count=%d" % count, ""]

out = os.path.join(unreal.SystemLibrary.get_project_directory(), "Saved", "meshes_out.txt")
with open(out, "w", encoding="utf-8") as handle:
    handle.write(chr(10).join(header + lines))

unreal.log("[scan_vehicle_meshes] wrote " + out)
