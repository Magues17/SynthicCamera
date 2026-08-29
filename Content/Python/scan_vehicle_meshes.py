"""
Report every static mesh under a content folder, with real-world dimensions.

Run this after adding vehicle assets from Fab. It writes Saved/meshes_out.txt with
each mesh's path and bounding size in centimetres, which is what the catalog needs
to map an asset onto a vehicle class - a 9m long mesh is a tank, a 5m one is not.

    powershell -File <skill>/scripts/run_ue_python.ps1 ^
        -Project SynthicCamera.uproject -Script Content/Python/scan_vehicle_meshes.py

Edit SCAN_ROOTS if the assets land somewhere other than /Game.
"""

import os

import unreal

SCAN_ROOTS = ["/Game"]
IGNORE_PREFIXES = ("/Game/Synthic", "/Game/Materials", "/Game/Developers", "/Game/Collections")

registry = unreal.AssetRegistryHelpers.get_asset_registry()
registry.scan_paths_synchronous(SCAN_ROOTS, True)

lines = []
for root in SCAN_ROOTS:
    for asset in registry.get_assets_by_path(root, recursive=True):
        path = str(asset.package_name)
        if path.startswith(IGNORE_PREFIXES):
            continue
        if str(asset.asset_class_path.asset_name) != "StaticMesh":
            continue

        mesh = unreal.EditorAssetLibrary.load_asset(path)
        if mesh is None:
            lines.append(f"{path}  <could not load>")
            continue

        # Bounds are half-extents in the mesh's own space, so double for real size.
        extent = mesh.get_bounds().box_extent
        lines.append(
            f"{path}\n"
            f"    size_cm  L={extent.x * 2:.0f}  W={extent.y * 2:.0f}  H={extent.z * 2:.0f}\n"
            f"    materials={mesh.get_num_sections(0)}")

header = [f"static_mesh_count={sum(1 for l in lines if l.startswith('/Game'))}", ""]
out = os.path.join(unreal.SystemLibrary.get_project_directory(), "Saved", "meshes_out.txt")
with open(out, "w", encoding="utf-8") as handle:
    handle.write(chr(10).join(header + lines))

unreal.log(f"[scan_vehicle_meshes] wrote {out}")
