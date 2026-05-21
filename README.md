# MeshSimplify — Nuke 14 NDK plugin

UV- and seam-aware mesh simplification for Nuke 14 (classic 3D system).
Wraps [meshoptimizer](https://github.com/zeux/meshoptimizer) (MIT) as a
`ModifyGeo` op.

## Build (Windows 11, Nuke 14)

```powershell
cmake -G "Visual Studio 16 2019" -A x64 `
      -DCMAKE_PREFIX_PATH="C:/Program Files/Nuke14.0v5" `
      -DCMAKE_BUILD_TYPE=Release `
      -B build
cmake --build build --config Release
copy build\Release\MeshSimplify.dll  $env:USERPROFILE\.nuke\
```

In Nuke: `Tab` → MeshSimplify, or right-click the node graph and look
under Geometry.

## Knobs

| Knob           | Default | Notes                                                 |
|----------------|---------|-------------------------------------------------------|
| target_ratio   | 0.5     | Fraction of triangles to keep. 1.0 = no change.       |
| target_error   | 0.01    | Max normalized deviation. Stops early if exceeded.    |
| uv_weight      | 1.0     | Penalty on UV distortion. 0 ignores UV.               |
| lock_borders   | off     | Protect boundary edges (open meshes that mate elsewhere). |
| aggressive     | off     | Sloppy simplifier: faster, ignores topology.          |

## Architecture

Input GeoInfo → fat vertices (unique (point_index, uv) tuples; this
exposes UV seams to meshoptimizer as topological discontinuities) →
`meshopt_simplifyWithAttributes` with UV as a weighted attribute pair →
fat-vertex compaction → rebuilt PointList + Triangle primitives +
per-corner UV attribute.

`meshopt_simplifySloppy` is selected when `aggressive` is on — fast LOD
generation, not seam-preserving.

## Testing strategy

The simplifier is pure and deterministic, so test by snapshot:

1. **A simple quad with a UV seam.** Two triangles, four points, but the
   UV uses 6 vertex slots because two points are on the seam (each has
   two UVs). Run with `target_ratio=1.0` (early-exit path; verify the
   mesh round-trips). Run with `target_ratio=0.5` (expect collapse to a
   degenerate but valid result, or skipped per the safety check).
2. **A textured sphere from GeoSphere.** Connect a Read with a checker
   texture, simplify to 0.3, view in 3D viewer with the texture applied.
   The checker pattern should remain continuous across the original UV
   seam at the sphere's longitudinal cut.
3. **A scanned mesh from a ReadGeo.** High-poly with no UVs (or with
   UVs and seams). Verify performance: 1M tris → ~50ms target on the
   non-aggressive path.

## Caveats / what to verify in your tree

These are the spots I had to make decisions without your specific
NDK headers in front of me. Quick verifications when you build:

1. **`Primitive::vertex_offset()` vs `vertex_offset_`.** Some NDK
   versions expose this as a public member, others as a function. If
   the build errors on `prim->vertex_offset()`, change to
   `prim->vertex_offset_` (member access).
2. **`Attribute::resize(n)` signature.** If the attribute API in your
   Nuke 14 minor version doesn't expose `resize`, the alternative is
   to delete and recreate the attribute via `writable_attribute` —
   that path always returns a fresh, correctly-sized attribute given
   the current primitive count.
3. **UV Vector4 vs Vector2.** Nuke historically stores UVs as Vector4
   (s, t, r, q). If your input mesh provides UVs as Vector2 instead,
   change `VECTOR4_ATTRIB` to `VECTOR2_ATTRIB` in both the read and
   write sides, and drop the `invq` unprojection step.
4. **`Op::Description` category string.** I used `"Geometry/MeshSimplify"`
   so the node lands under Geometry in the Tab menu. Adjust to taste.

## Known limitations

- **Per-vertex normals are dropped.** Nuke usually recomputes them
  downstream; if you need preserved normals, add a third attribute
  pair to `meshopt_simplifyWithAttributes` and write back a Group_Vertices
  "N" attribute.
- **Per-vertex colors and arbitrary attributes are dropped.** Same
  pattern as normals.
- **Concave n-gons fan-triangulate to bad geometry.** Nuke rarely
  produces these; if your inputs include them, swap the fan
  triangulator for an ear-clipper.
- **`lock_borders` only applies to the non-aggressive path.** The
  sloppy simplifier doesn't expose a border-lock option.

## License

Plugin code: MIT.
meshoptimizer: MIT (https://github.com/zeux/meshoptimizer).
Nuke NDK: Foundry's licence terms apply to the resulting `.dll` only
when linked against `DDImage`.
