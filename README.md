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
| `Source/SynthicCamera/Synth/SynthCaptureDirector.*` | Seeded run controller: N randomised passes |
| `Content/Python/build_desert.py` | Rebuilds the whole scene from scratch |

## Output

`Saved/SynthData/<run>/` — `labels.jsonl` alongside `<run>_%06d.png`.

Each label row carries vehicle identity and geometry, true / commanded / radar speed,
the radar cosine angle, the 2D bounding box with validity and clipping flags, camera
pose, and sun angle.

## Rebuilding

```
powershell -File <skill>/scripts/build_ue.ps1    -Project SynthicCamera.uproject -Target SynthicCameraEditor
powershell -File <skill>/scripts/run_ue_python.ps1 -Project SynthicCamera.uproject -Script Content/Python/build_desert.py
```

## Status

Vehicles are proxy boxes with real-world dimensions, marked `PROXY` in the make field.
Point a catalog entry's `Mesh` at an imported asset to promote it — the mesh is a data
field, not a code path.
