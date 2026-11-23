# MeshSimplify: Nuke NDK plugin (Nuke 14.1 - 17.1, Windows + Linux)

Crop classic-3D geometry to a box / Cube and to a camera frustum, then
UV- and seam-aware simplification with
[meshoptimizer](https://github.com/zeux/meshoptimizer) (MIT), optionally
merging flat triangle pairs into quads. A `GeoOp` (topology changes; see
NDK_NOTES 3.1).

## Build

Windows (VS2019 x64, every installed Nuke in one go; one `build-<ver>/` each):

```powershell
scripts\build.bat                 # = powershell -ExecutionPolicy Bypass -File scripts\build_all.ps1
scripts\build.bat -Install        # + cmake --install into %USERPROFILE%\.nuke
scripts\build.bat -Versions 16.0v8 -Clean
.\deploy.ps1 -Install -Package -Version 1.1.0   # build + install + zip
```

Versions built by default: 14.1v8, 15.0v8, 15.1v10, 15.2v9, 16.0v8, 16.1v1,
17.0v4, 17.1v1 (`C:\Program Files\Nuke<ver>`). Linux: see
[docs/LINUX_BUILD.md](docs/LINUX_BUILD.md).

Install layout (`cmake --install <build> --prefix ~/.nuke`):

```
.nuke/init.py                      # + nuke.pluginAddPath('./MeshSimplify')
.nuke/MeshSimplify/init.py         # adds <major.minor>/ for the running Nuke
.nuke/MeshSimplify/menu.py         # 3D > Modify > MeshSimplify
.nuke/MeshSimplify/14.1/MeshSimplify.dll (.so on Linux)
...
```

The install step also removes the v1.0 layout: `MeshSimplify.dll` directly in
`.nuke/` or `.nuke/MeshSimplify/` (either would be loaded by every Nuke
version ahead of the right one) and the old `pluginAddPath` block that
`deploy.ps1` wrote into `.nuke/menu.py`.

## Tests

```powershell
powershell -ExecutionPolicy Bypass -File scripts\test_all.ps1 [-Perf]
```

Runs `test/test_crop.py` headless (`-ti`) against each FRESH build dir: exact
face counts for box / Cube / Axis crops, the frustum (aspect, overscan,
window translate, rotated camera, near / far, behind-camera), the frame-range
union and its hash (an off-frame key edit re-cooks), crop + simplify, object
transform and uv survival. `test/perf.py` times a ~500k-triangle sphere.

## Inputs

| Input | |
|---|---|
| 0 (unlabelled) | geometry to process |
| `cam` | optional classic camera: crop to its frustum |
| `crop` | optional: a Cube / any geometry (each object's oriented bounding box) or an Axis (places `crop box`) |

## Knobs

| Knob | Default | Notes |
|---|---|---|
| target_ratio | 0.5 | Fraction of triangles to keep (after cropping). 1.0 = no simplification. |
| target_error | 0.01 | Max normalized deviation. Stops early if exceeded. |
| uv_weight | 1.0 | Penalty on UV distortion. 0 ignores UV. |
| lock_borders | off | Protect open boundaries, including the edge a crop leaves. |
| pin_silhouette / silhouette_angle | off / 30 | Lock vertices on sharp creases. |
| aggressive | off | Sloppy simplifier: faster, ignores topology. |
| ignore_uv_seams, prune_components | off | Flat-area handling. |
| output_quads / quad_angle | off / 5 | Merge coplanar triangle pairs into quads. |
| crop_camera | on | Crop to the `cam` frustum (no effect while `cam` is disconnected). |
| overscan | 0 | Pixels added on every side of the script format (ScanlineRender units). |
| clip_near_far | on | Also crop to the camera's near / far planes. |
| crop_frames | current frame | `frame range`: keep faces seen at ANY sampled frame. |
| use_script_range, crop_first / crop_last, crop_step | on, 1-100, 1 | Frames sampled in `frame range` mode. |
| crop_box_enable / crop_box | off / unit box | Box crop (see the `crop` input). |
| crop_test | touching | `touching` keeps faces that may overlap the region; `inside` only faces entirely inside. |

### Crop details

* Faces are kept or dropped whole, never cut. `touching` drops a face only
  when all its corners lie outside one same plane of the region, so a big
  ground-plane face spanning the frame survives.
* Box AND camera: a face must pass both. Several crop objects or frames: a
  face is kept if any of them keeps it.
* Cropping runs before simplification, so the triangle budget goes to what
  remains. With target_ratio 1 and quads off, kept faces pass through with
  their original polygon layout (quads stay quads).
* The camera is evaluated per frame through `node_input()` at that frame's
  context, so parenting / look-at are honoured; lens values come from the
  camera's knobs at that frame. Perspective cameras only. The geometry and the
  crop box are taken at the current frame.
* The resolved planes for every sampled frame go into the geometry hash, so
  editing a camera key away from the current frame re-cooks. Cost: the camera
  is validated at every sampled frame on each validate (measured ~0.5 s for
  1000 frames of an animated camera on a 40k-face card; a static camera
  collapses to one frustum, 0.02 s). Raise `crop_step` for long shots.
* An unusable camera (orthographic, zero focal) skips the camera crop with a
  warning instead of removing everything.

## Architecture

Input GeoInfo -> faces as flat corner lists (point + uv) -> crop flags
(per-volume vertex outside-bitmasks, AND / OR per face) -> either pass-through
of kept faces, or fan triangulation -> fat vertices (unique (point, uv) via
`meshopt_generateVertexRemap`; UV seams become topological breaks) ->
`meshopt_simplifyWithAttributes` (or `meshopt_simplifySloppy`) -> per-point
compaction -> optional quad merge (sorted edge list) -> rebuilt PointList +
Triangle / Polygon primitives + per-corner uv. Each object's matrix,
material, render / display mode and selectable flag are restored after the
rebuild (they were lost before 1.1, undoing an upstream TransformGeo).

## Known limitations

- Per-vertex normals, colours and other attributes are dropped (uv is kept).
- Concave n-gons fan-triangulate to bad geometry when simplifying.
- `lock_borders` only applies to the non-aggressive path.
- The 3D viewer does not draw the crop box or frustum; view the `crop` /
  `cam` nodes alongside.

## License

Plugin code: MIT. meshoptimizer: MIT (https://github.com/zeux/meshoptimizer),
reproduced in LICENSE, which ships with every package.
