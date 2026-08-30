# SynthicCamera

Synthetic training data for military vehicle recognition, generated in Unreal Engine 5.8.

A roadside speed camera in a desert scene photographs vehicles as they pass and emits
a labelled sample per pass: one PNG plus one JSONL row of ground truth.

## The idea

The camera does not detect anything. The engine already knows each vehicle's exact
class, dimensions, pose and velocity, so the label is a *read*, not an inference.
That label file is what a model is later trained to reproduce from the image alone,
and what it gets validated against on real speed-camera footage.

## Layout

| Path | What |
| --- | --- |
| `Source/SynthicCamera/Synth/SynthTypes.h` | Vehicle catalog entry: make, model, class, dimensions, mesh, livery |
| `Source/SynthicCamera/Synth/SynthVehicle.*` | Kinematic vehicle; drives straight at an exactly-known speed |
| `Source/SynthicCamera/Synth/SynthSpeedCamera.*` | Scene capture + trigger; writes the PNG and the label row |
| `Source/SynthicCamera/Synth/SynthProjection.*` | Pure 3D-bounds-to-2D-bbox maths |
| `Source/SynthicCamera/Synth/SynthDataset.*` | The only filesystem I/O in the module |
| `Source/SynthicCamera/Synth/SynthWeather.*` | Atmospheric presets and how they reach the capture |
| `Source/SynthicCamera/Synth/SynthCaptureDirector.*` | Seeded run controller: N randomised passes |
| `Content/Python/build_desert.py` | Rebuilds the whole scene from scratch |
| `tools/preview_labels.py` | Draws each label back onto its image - the end-to-end check |

## Output

`Saved/SynthData/<run>/` — `labels.jsonl` alongside `<run>_%06d.png`.

Each label row carries vehicle identity and geometry, true / commanded / radar speed,
the radar cosine angle, the 2D bounding box with validity and clipping flags, camera
pose, and sun angle.

## Assets

Marketplace content is licensed for use in a project but not for redistribution, so
it is not committed. A fresh clone needs these added through the editor's Fab browser
(Window > Fab), which places them at the paths the catalog expects:

| Pack | Lands at | Used for |
| --- | --- | --- |
| Vehicle Variety Pack | `Content/VehicleVarietyPack/` | Hatchback, SportsCar, SUV, Pickup, BoxTruck |
| RTS Combat Vehicle | `Content/Fab/RTS_Combat_Vehicle/` | CombatATV |
| Sci-fi Armored Vehicle (rigged) | `Content/Fab/Sci_fi_Armored_Vehicle_rigged/` | ArmoredVehicle |
| Willys mountain buggy | `Content/Fab/Willys_mountain_buggy_1/` | WillysBuggy |
| ZIL-130 body (lowpoly) | `Content/Fab/ZIL_130__body____Lowpoly/` | ZIL130Truck |

Without them the affected vehicles fall back to proxy boxes and each one warns at
startup naming the mesh it could not load. The run still completes, so check the log
rather than trusting that clean output means a complete setup.

After adding assets, confirm what actually landed and how it is oriented:

```
powershell -File <skill>/scripts/run_ue_python.ps1 -Project SynthicCamera.uproject ^
    -Script Content/Python/scan_vehicle_meshes.py
```

That reports every static mesh with its real-world size and pivot. Both matter: several
packs author the vehicle facing along Y, which drives it sideways down the road while
reporting entirely sensible dimensions, and some put the pivot at the model centre,
which buries it half its height in the tarmac. Both are corrected per entry in the
catalog via `MeshRotation` and `MeshOffsetCm`.

## Rebuilding

Compile the module, then rebuild the scene. The scene script is idempotent - run it
again after changing camera geometry and it prunes and rebuilds in place.

```
powershell -File <skill>/scripts/build_ue.ps1      -Project SynthicCamera.uproject -Target SynthicCameraEditor
powershell -File <skill>/scripts/run_ue_python.ps1 -Project SynthicCamera.uproject -Script Content/Python/build_desert.py
```

## Generating a dataset

Headless, exits by itself when the run completes:

```
"C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" SynthicCamera.uproject /Game/Synthic/Lvl_Desert -game -RenderOffScreen -unattended -nosplash -SynthAutoQuit
```

Then check the labels actually line up with the pixels:

```
python tools/preview_labels.py Saved/SynthData/<run>
```

It reports bbox validity, class and weather balance, the randomisation spread per
axis, and target contrast - how far the vehicle sits from its background. Heavy fog
can wash a vehicle out until it is not meaningfully visible while its label stays
confident, and those samples are named so they can be dropped or the fog capped.

Tune passes, seed, and speed range on the `CaptureDirector` actor; camera height,
lateral offset and aim point live in `build_desert.py`.

## Checks

The bounding box is projected from 3D bounds, never measured from pixels, so a wrong
box still produces plausible-looking numbers. Two things guard it:

```
"C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" SynthicCamera.uproject -ExecCmds="Automation RunTests Synthic.Projection;quit" -unattended -nopause -nosplash
```

and the visual overlay from `tools/preview_labels.py`.

## Status

Vehicles are proxy boxes with real-world dimensions, marked `PROXY` in the make field.
Point a catalog entry's `Mesh` at an imported asset to promote it — the mesh is a data
field, not a code path.
