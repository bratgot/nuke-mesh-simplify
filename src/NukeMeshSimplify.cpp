// NukeMeshSimplify.cpp
//
// Nuke NDK plugin (classic 3D, Nuke 14.1 - 17.1, Windows + Linux).
// Crop to a box / cube and / or a camera frustum (optionally the union of the
// frustum over an animated frame range), then simplify (meshoptimizer, MIT),
// then optionally merge coplanar triangle pairs into quads.
//
// Inputs:
//   0  geometry to process
//   1  cam   (optional) classic camera: crop to its frustum
//   2  crop  (optional) a Cube / any geometry (crop to each object's oriented
//            bounding box) or an Axis (crop to 'crop box' placed by the axis)
//
// Key knobs:
//   target_ratio, target_error, uv_weight   -- core simplifier controls
//   ignore_uv_seams                         -- bypass UV-seam preservation
//   prune_components                        -- drop small disconnected pieces
//   output_quads, quad_angle                -- merge coplanar tri pairs
//   crop_*                                  -- box / frustum crop

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef _USE_MATH_DEFINES
#  define _USE_MATH_DEFINES
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#ifdef _WIN32
#  include <windows.h>
#  ifdef POINTS
#    undef POINTS
#  endif
#endif

#include "DDImage/GeoOp.h"
#include "DDImage/GeoInfo.h"
#include "DDImage/GeometryList.h"
#include "DDImage/Scene.h"
#include "DDImage/Triangle.h"
#include "DDImage/Polygon.h"
#include "DDImage/Knobs.h"
#include "DDImage/Knob.h"
#include "DDImage/Vector4.h"
#include "DDImage/Vector3.h"
#include "DDImage/Vector2.h"
#include "DDImage/Matrix4.h"
#include "DDImage/Attribute.h"
#include "DDImage/Hash.h"
#include "DDImage/AxisOp.h"
#include "DDImage/CameraOp.h"
#include "DDImage/Format.h"

#include <meshoptimizer.h>

#include <vector>
#include <string>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <exception>

using namespace DD::Image;

namespace {

static const char* const kClass = "MeshSimplify";

static const char* const kHelp =
    "<b>MeshSimplify</b><br>"
    "<i>Crop, UV-aware mesh decimation and optional quad-pair merging for Nuke 14.1 - 17.1.</i><br>"
    "Created by Marten Blumen.<br>"
    "<br>"
    "<b>Inputs</b><br>"
    "<b>cam</b> (optional): crop to the camera frustum, widened by 'overscan' pixels "
    "on each side of the script format. With 'frames' set to 'frame range' the "
    "frustum is sampled every 'step' frames and a face is kept when ANY sample sees "
    "it, so the result covers the whole camera move.<br>"
    "<b>crop</b> (optional): a Cube (or any geometry) crops to each object's oriented "
    "bounding box; an Axis places the 'crop box'. With nothing connected 'crop box' is "
    "in world space.<br>"
    "<br>"
    "<b>Crop test</b><br>"
    "'touching' keeps every face that may overlap the region (a face is only dropped "
    "when all its vertices lie outside one plane of the region, so large faces that "
    "span the frame survive). 'inside' keeps only faces whose vertices are all inside. "
    "Faces are kept whole, never cut. Cropping runs before simplification, so the "
    "triangle budget is spent on what remains. With target ratio 1 and quads off, "
    "kept faces are passed through with their original polygon layout.<br>"
    "<br>"
    "<b>Algorithm</b><br>"
    "Quadric edge-collapse simplification via meshoptimizer (MIT). A fat-vertex bridge<br>"
    "keys unique (point, UV) pairs so texture seams become topological boundaries<br>"
    "the simplifier preserves automatically. Optional feature-edge detection locks vertices on<br>"
    "sharp creases. After simplification, an optional coplanar triangle-pair merger promotes<br>"
    "flat areas to quad Polygon primitives, only crossing edges where UVs are continuous.<br>"
    "<br>"
    "<b>Usage notes</b><br>"
    "For open meshes (and crop boundaries) use 'lock borders' to keep the boundary loop "
    "intact. For closed meshes like a cube, use 'pin silhouette' to keep sharp corners. "
    "'output quads' is a pair-merger, not a quad remesher. The 'aggressive' path is faster "
    "but ignores both UV awareness and the silhouette pin. Only the uv attribute is "
    "carried through; other per-vertex attributes (N, Cf) are dropped.";

static const char* const kCropModes[]  = { "touching", "inside", nullptr };
static const char* const kFrameModes[] = { "current frame", "frame range", nullptr };
enum { CROP_TOUCHING = 0, CROP_INSIDE = 1 };
enum { FRAMES_CURRENT = 0, FRAMES_RANGE = 1 };

// ---------------------------------------------------------------------------
// Small double-precision 4x4 helpers. Column-major like DD::Image::Matrix4 and
// fdk::Mat4d: element (row r, col c) = m[c*4 + r].
// ---------------------------------------------------------------------------
struct M4 {
    double m[16];
    double at(int r, int c) const { return m[c * 4 + r]; }
};

static M4 m4_from_float(const float* a)
{
    M4 r;
    for (int i = 0; i < 16; ++i) r.m[i] = static_cast<double>(a[i]);
    return r;
}

static M4 m4_from_double(const double* a)
{
    M4 r;
    for (int i = 0; i < 16; ++i) r.m[i] = a[i];
    return r;
}

static bool m4_inverse(const M4& A, M4& out)
{
    const double* a = A.m;
    double inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] +
             a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] -
             a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] +
             a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] -
              a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] -
             a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] +
             a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] -
             a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] +
              a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] +
             a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] -
             a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] +
              a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] -
              a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] -
             a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] +
             a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] -
              a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] +
              a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (!(std::fabs(det) > 1e-300)) return false;
    const double s = 1.0 / det;
    for (int i = 0; i < 16; ++i) out.m[i] = inv[i] * s;
    return true;
}

// Half-space a*x + b*y + c*z + d >= 0 is "inside".
struct Plane { double v[4]; };

// Plane given in space A, points of space B map into A by p_A = T * p_B.
// Returns the same plane expressed in space B (row vector times matrix).
static Plane plane_through(const Plane& p, const M4& T)
{
    Plane r;
    for (int j = 0; j < 4; ++j) {
        r.v[j] = p.v[0] * T.at(0, j) + p.v[1] * T.at(1, j) +
                 p.v[2] * T.at(2, j) + p.v[3] * T.at(3, j);
    }
    return r;
}

static Plane make_plane(double a, double b, double c, double d)
{
    Plane p; p.v[0] = a; p.v[1] = b; p.v[2] = c; p.v[3] = d;
    return p;
}

// Convex region = intersection of up to 16 half-spaces (world space).
struct Volume { std::vector<Plane> planes; };

static bool same_volume(const Volume& a, const Volume& b)
{
    if (a.planes.size() != b.planes.size()) return false;
    for (size_t i = 0; i < a.planes.size(); ++i)
        for (int j = 0; j < 4; ++j)
            if (std::fabs(a.planes[i].v[j] - b.planes[i].v[j]) > 1e-9 * (1.0 + std::fabs(a.planes[i].v[j])))
                return false;
    return true;
}

// Six planes of an axis-aligned box [lo, hi] in its own space, then moved to
// world by box_to_world.
static Volume box_volume(const double lo[3], const double hi[3], const M4& box_to_world)
{
    M4 inv;
    Volume vol;
    if (!m4_inverse(box_to_world, inv)) return vol;
    const Plane local[6] = {
        make_plane( 1, 0, 0, -lo[0]), make_plane(-1, 0, 0, hi[0]),
        make_plane( 0, 1, 0, -lo[1]), make_plane( 0,-1, 0, hi[1]),
        make_plane( 0, 0, 1, -lo[2]), make_plane( 0, 0,-1, hi[2]),
    };
    // p_box = inv * p_world
    for (const Plane& p : local) vol.planes.push_back(plane_through(p, inv));
    return vol;
}

static double knob_at(Op* op, const char* name, double t, int idx, double fallback)
{
    Knob* k = op ? op->knob(name) : nullptr;
    return k ? k->get_value_at(t, idx) : fallback;
}

// ---------------------------------------------------------------------------
// Sorted edge list: replaces unordered_map<edge, vector<ref>> (one heap
// allocation per edge) with one flat sort. Runs of equal key are the faces
// sharing an edge; entries within a run are in triangle order, so results are
// deterministic.
// ---------------------------------------------------------------------------
struct EdgeEntry {
    uint64_t key;
    uint32_t tri;
    uint32_t edge;
};

static void build_sorted_edges(const uint32_t* idx, size_t num_tris, std::vector<EdgeEntry>& out)
{
    out.resize(num_tris * 3);
    for (size_t t = 0; t < num_tris; ++t) {
        for (uint32_t e = 0; e < 3; ++e) {
            uint32_t a = idx[t * 3 + e];
            uint32_t b = idx[t * 3 + ((e + 1) % 3)];
            if (a > b) std::swap(a, b);
            EdgeEntry& ee = out[t * 3 + e];
            ee.key  = (static_cast<uint64_t>(a) << 32) | static_cast<uint64_t>(b);
            ee.tri  = static_cast<uint32_t>(t);
            ee.edge = e;
        }
    }
    std::sort(out.begin(), out.end(), [](const EdgeEntry& x, const EdgeEntry& y) {
        return x.key != y.key ? x.key < y.key : x.tri < y.tri;
    });
}

// Calls fn(first, second) for every edge shared by exactly two triangles.
template <class Fn>
static void for_each_manifold_edge(const std::vector<EdgeEntry>& edges, Fn fn)
{
    const size_t n = edges.size();
    size_t i = 0;
    while (i < n) {
        size_t j = i + 1;
        while (j < n && edges[j].key == edges[i].key) ++j;
        if (j - i == 2) fn(edges[i], edges[i + 1]);
        i = j;
    }
}

static Vector3 tri_normal(const Vector3& a, const Vector3& b, const Vector3& c)
{
    Vector3 n = (b - a).cross(c - a);
    const float len = n.length();
    if (len > 0.0f) n /= len;
    return n;
}

// ---------------------------------------------------------------------------
// Output of processObject(). Triangles and polygons coexist; polygons are
// either merged quads or (crop-only path) the input's own n-gons. Write-back
// emits all triangles first, then all polygons; UVs follow the same order.
// ---------------------------------------------------------------------------
struct OutMesh {
    std::vector<Vector3>  points;
    std::vector<uint32_t> tri_indices;    // 3 per tri
    std::vector<Vector4>  tri_uvs;        // 3 per tri (per-corner)
    std::vector<uint32_t> poly_sizes;     // corners per polygon
    std::vector<uint32_t> poly_indices;   // concatenated corners
    std::vector<Vector4>  poly_uvs;       // per corner
    bool has_uv = false;
    bool valid  = false;
    bool collapsed = false;
    size_t faces_in = 0, faces_kept = 0;
};

// Per-object state copied before delete_objects() and restored afterwards:
// a rebuilt GeoInfo starts with an identity matrix and no material, which
// would silently undo an upstream TransformGeo / ApplyMaterial.
struct ObjState {
    Matrix4 matrix;
    Iop*    material = nullptr;
    DD::Image::RenderMode    render_mode{};
    DD::Image::Display3DMode display3d{};
    bool    selectable = true;
};

// ---------------------------------------------------------------------------
// Quad-pair merger. Greedy: most planar candidate edges first; an edge is a
// candidate when shared by exactly two triangles, their dihedral angle is under
// the threshold and UVs match on both sides. Quad corner order [opp1, a, opp2, b]
// keeps T1's winding.
// ---------------------------------------------------------------------------
static void mergeCoplanarTriPairs(
    std::vector<uint32_t>& tri_indices,
    std::vector<Vector4>&  tri_uvs,
    std::vector<uint32_t>& poly_sizes,
    std::vector<uint32_t>& poly_indices,
    std::vector<Vector4>&  poly_uvs,
    const std::vector<Vector3>& points,
    float angle_threshold_rad,
    bool  has_uv)
{
    const size_t num_tris = tri_indices.size() / 3;
    if (num_tris < 2) return;

    std::vector<Vector3> normals(num_tris);
    for (size_t t = 0; t < num_tris; ++t) {
        normals[t] = tri_normal(points[tri_indices[t * 3 + 0]],
                                points[tri_indices[t * 3 + 1]],
                                points[tri_indices[t * 3 + 2]]);
    }

    std::vector<EdgeEntry> edges;
    build_sorted_edges(tri_indices.data(), num_tris, edges);

    struct Candidate { uint32_t t1, t2, e1; float dot; };
    std::vector<Candidate> candidates;
    candidates.reserve(num_tris);
    const float cos_threshold = std::cos(angle_threshold_rad);

    auto uv_close = [](const Vector4& a, const Vector4& b) {
        const float du = a.x - b.x;
        const float dv = a.y - b.y;
        return (du * du + dv * dv) < 1.0e-10f;
    };

    for_each_manifold_edge(edges, [&](const EdgeEntry& r1, const EdgeEntry& r2) {
        const float d = normals[r1.tri].dot(normals[r2.tri]);
        if (d < cos_threshold) return;
        if (has_uv) {
            const Vector4& uv_t1_a = tri_uvs[r1.tri * 3 + r1.edge];
            const Vector4& uv_t1_b = tri_uvs[r1.tri * 3 + ((r1.edge + 1) % 3)];
            // T2 runs the shared edge the other way round.
            const Vector4& uv_t2_b = tri_uvs[r2.tri * 3 + r2.edge];
            const Vector4& uv_t2_a = tri_uvs[r2.tri * 3 + ((r2.edge + 1) % 3)];
            if (!uv_close(uv_t1_a, uv_t2_a)) return;
            if (!uv_close(uv_t1_b, uv_t2_b)) return;
        }
        candidates.push_back({ r1.tri, r2.tri, r1.edge, d });
    });

    std::stable_sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) { return a.dot > b.dot; });

    std::vector<uint8_t> consumed(num_tris, 0);
    for (const Candidate& c : candidates) {
        if (consumed[c.t1] || consumed[c.t2]) continue;
        const uint32_t a   = tri_indices[c.t1 * 3 + c.e1];
        const uint32_t b   = tri_indices[c.t1 * 3 + ((c.e1 + 1) % 3)];
        const uint32_t op1 = tri_indices[c.t1 * 3 + ((c.e1 + 2) % 3)];
        int k2 = 0;
        for (int k = 0; k < 3; ++k) {
            const uint32_t v = tri_indices[c.t2 * 3 + k];
            if (v != a && v != b) { k2 = k; break; }
        }
        const uint32_t op2 = tri_indices[c.t2 * 3 + k2];

        poly_sizes.push_back(4);
        poly_indices.push_back(op1);
        poly_indices.push_back(a);
        poly_indices.push_back(op2);
        poly_indices.push_back(b);
        if (has_uv) {
            poly_uvs.push_back(tri_uvs[c.t1 * 3 + ((c.e1 + 2) % 3)]);
            poly_uvs.push_back(tri_uvs[c.t1 * 3 + c.e1]);
            poly_uvs.push_back(tri_uvs[c.t2 * 3 + k2]);
            poly_uvs.push_back(tri_uvs[c.t1 * 3 + ((c.e1 + 1) % 3)]);
        }
        consumed[c.t1] = 1;
        consumed[c.t2] = 1;
    }

    // Repack unconsumed triangles in place.
    size_t w = 0;
    for (size_t t = 0; t < num_tris; ++t) {
        if (consumed[t]) continue;
        for (int k = 0; k < 3; ++k) {
            tri_indices[w * 3 + k] = tri_indices[t * 3 + k];
            if (has_uv) tri_uvs[w * 3 + k] = tri_uvs[t * 3 + k];
        }
        ++w;
    }
    tri_indices.resize(w * 3);
    if (has_uv) tri_uvs.resize(w * 3);
}

// ---------------------------------------------------------------------------

class MeshSimplify : public GeoOp {
public:
    MeshSimplify(Node* node) : GeoOp(node) {
        k_target_ratio    = 0.5f;
        k_target_error    = 0.01f;
        k_uv_weight       = 1.0f;
        k_lock_borders    = false;
        k_aggressive      = false;
        k_ignore_uv_seams = false;
        k_prune_components = false;
        k_output_quads    = false;
        k_quad_angle      = 5.0f;
        k_pin_silhouette   = false;
        k_silhouette_angle = 30.0f;

        k_crop_box_on   = false;
        k_crop_cam_on   = true;
        k_crop_mode     = CROP_TOUCHING;
        k_crop_box[0] = k_crop_box[1] = k_crop_box[2] = -0.5f;
        k_crop_box[3] = k_crop_box[4] = k_crop_box[5] =  0.5f;
        k_overscan      = 0.0f;
        k_clip_near_far = true;
        k_frame_mode    = FRAMES_CURRENT;
        k_use_script_range = true;
        k_first_frame   = 1;
        k_last_frame    = 100;
        k_frame_step    = 1;
    }

    const char* Class() const override { return kClass; }
    const char* node_help() const override { return kHelp; }

    // Three arrows always drawn (NDK_NOTES 28); cam and crop are optional and
    // default to nothing, so "disconnected" means "no crop".
    int minimum_inputs() const override { return 3; }
    int maximum_inputs() const override { return 3; }
    float uses_input(int) const override { return 1.0f; }

    Op* default_input(int n) const override {
        if (n == 0) return GeoOp::default_input(n);
        return nullptr;
    }

    bool test_input(int n, Op* op) const override {
        if (n == 0) return dynamic_cast<GeoOp*>(op) != nullptr;
        if (n == 1) return op == nullptr || dynamic_cast<CameraOp*>(op) != nullptr;
        if (n == 2) return op == nullptr || dynamic_cast<GeoOp*>(op) != nullptr ||
                           dynamic_cast<AxisOp*>(op) != nullptr;
        return false;
    }

    const char* input_label(int n, char*) const override {
        if (n == 1) return "cam";
        if (n == 2) return "crop";
        return "";
    }

    void knobs(Knob_Callback f) override {
        GeoOp::knobs(f);

        Float_knob(f, &k_target_ratio, "target_ratio", "target ratio");
        SetRange(f, 0.0, 1.0);
        Tooltip(f, "Fraction of triangles to keep (after cropping). 1.0 leaves "
                   "the mesh unsimplified.");

        Float_knob(f, &k_target_error, "target_error", "max error");
        SetRange(f, 0.0, 0.1);
        Tooltip(f, "Maximum allowed normalized deviation from the original "
                   "surface. The simplifier stops early once a collapse "
                   "would exceed this.");

        Float_knob(f, &k_uv_weight, "uv_weight", "uv weight");
        SetRange(f, 0.0, 4.0);
        Tooltip(f, "Penalty on UV distortion. Higher = less UV stretch, "
                   "less aggressive collapse.");

        Bool_knob(f, &k_lock_borders, "lock_borders", "lock borders");
        Tooltip(f, "Protect open mesh boundaries (including the edge left by "
                   "a crop) from collapse. No effect on closed meshes -- use "
                   "'pin silhouette' for those.");

        Bool_knob(f, &k_pin_silhouette, "pin_silhouette", "pin silhouette");
        Tooltip(f, "Preserve the overall shape by locking vertices on feature "
                   "edges (where adjacent faces meet at a sharp angle). Not "
                   "supported by the aggressive simplifier.");

        Float_knob(f, &k_silhouette_angle, "silhouette_angle", "silhouette angle");
        SetRange(f, 0.0, 90.0);
        Tooltip(f, "Dihedral angle threshold in degrees for 'pin silhouette'.");

        Bool_knob(f, &k_aggressive, "aggressive", "aggressive");
        Tooltip(f, "Sloppy simplifier: faster, ignores topology, lower "
                   "quality. UV-aware mode is disabled in this path.");

        Divider(f, "flat-area handling");

        Bool_knob(f, &k_ignore_uv_seams, "ignore_uv_seams", "ignore uv seams");
        Tooltip(f, "Treat the mesh as connected regardless of UV seams. Allows "
                   "more collapse of flat areas split across per-face UV "
                   "mappings (e.g. cube faces). UV continuity across seams is "
                   "no longer guaranteed.");

        Bool_knob(f, &k_prune_components, "prune_components", "prune small components");
        Tooltip(f, "Allow the simplifier to remove small disconnected mesh "
                   "components entirely.");

        Divider(f, "quad merging");

        Bool_knob(f, &k_output_quads, "output_quads", "output quads");
        Tooltip(f, "After simplification, merge coplanar triangle pairs that "
                   "share an edge (with continuous UVs) into Polygon quads.");

        Float_knob(f, &k_quad_angle, "quad_angle", "max quad angle");
        SetRange(f, 0.0, 45.0);
        Tooltip(f, "Maximum dihedral angle in degrees between two triangles "
                   "for them to be merged into a quad.");

        Divider(f, "crop");

        Bool_knob(f, &k_crop_cam_on, "crop_camera", "crop to camera");
        Tooltip(f, "Keep only faces inside the frustum of the camera on the "
                   "'cam' input. No effect while 'cam' is disconnected.");

        Float_knob(f, &k_overscan, "overscan", "overscan");
        SetRange(f, 0.0, 500.0);
        Tooltip(f, "Widen the frustum by this many pixels on every side of the "
                   "script format (same units as ScanlineRender's overscan).");

        Bool_knob(f, &k_clip_near_far, "clip_near_far", "clip to near / far");
        Tooltip(f, "Also crop to the camera's near and far clipping planes. "
                   "Off: everything in front of the camera is kept, at any "
                   "distance.");

        Enumeration_knob(f, &k_frame_mode, kFrameModes, "crop_frames", "frames");
        Tooltip(f, "current frame: crop to the frustum at the frame being "
                   "rendered. frame range: keep faces seen by the camera at ANY "
                   "sampled frame of the range, so one cropped mesh serves the "
                   "whole shot.");

        Bool_knob(f, &k_use_script_range, "use_script_range", "use script range");
        Tooltip(f, "Sample the script's frame range (Project Settings).");

        Int_knob(f, &k_first_frame, "crop_first", "range");
        Int_knob(f, &k_last_frame, "crop_last", "");
        ClearFlags(f, Knob::STARTLINE);
        Int_knob(f, &k_frame_step, "crop_step", "step");
        ClearFlags(f, Knob::STARTLINE);
        Tooltip(f, "Sample every Nth frame. Fast camera moves may need step 1 "
                   "or some overscan so faces seen only between samples are "
                   "not lost.");

        Bool_knob(f, &k_crop_box_on, "crop_box_enable", "crop to box");
        SetFlags(f, Knob::STARTLINE);
        Tooltip(f, "Keep only faces inside a box: each object of the geometry "
                   "on the 'crop' input (its oriented bounding box -- use a "
                   "Cube), or 'crop box' placed by an Axis on 'crop', or "
                   "'crop box' in world space when 'crop' is disconnected.");

        Box3_knob(f, k_crop_box, "crop_box", "crop box");
        Tooltip(f, "Box extents (x y z min, x y z max). Ignored when geometry "
                   "is connected to 'crop'.");

        Enumeration_knob(f, &k_crop_mode, kCropModes, "crop_test", "keep faces");
        Tooltip(f, "touching: keep faces that may overlap the region "
                   "(conservative). inside: keep only faces entirely inside.");
    }

    void enable_knob(const char* name, bool on) {
        if (Knob* k = knob(name)) k->enable(on);
    }

    int knob_changed(Knob* k) override {
        if (k->is("use_script_range") || k->is("crop_frames") || k->is("crop_camera") ||
            k->is("crop_box_enable") || k->is("showPanel")) {
            const bool range = k_frame_mode == FRAMES_RANGE;
            enable_knob("crop_first", range && !k_use_script_range);
            enable_knob("crop_last", range && !k_use_script_range);
            enable_knob("crop_step", range);
            enable_knob("use_script_range", range);
            enable_knob("crop_box", k_crop_box_on);
            return 1;
        }
        return GeoOp::knob_changed(k);
    }

    void get_geometry_hash() override {
        GeoOp::get_geometry_hash();

        build_crop();

        // Every knob can change topology, points and uvs; fold them and the
        // resolved crop planes (camera at every sampled frame -- the camera's
        // own hash only covers the current frame) into every group.
        Hash h;
        h.append(k_target_ratio);
        h.append(k_target_error);
        h.append(k_uv_weight);
        h.append(k_lock_borders);
        h.append(k_aggressive);
        h.append(k_ignore_uv_seams);
        h.append(k_prune_components);
        h.append(k_output_quads);
        h.append(k_quad_angle);
        h.append(k_pin_silhouette);
        h.append(k_silhouette_angle);
        h.append(k_crop_mode);
        h.append(crop_active_);
        h.append(int(boxes_.size()));
        h.append(int(frusta_.size()));
        h.append(have_box_);
        h.append(have_cam_);
        auto add_vols = [&](const std::vector<Volume>& vols) {
            for (const Volume& v : vols)
                for (const Plane& p : v.planes)
                    for (int j = 0; j < 4; ++j) h.append(p.v[j]);
        };
        add_vols(boxes_);
        add_vols(frusta_);

        for (int g = 0; g < Group_Last; ++g)
            geo_hash[g].append(h);
    }

    void geometry_engine(Scene& scene, GeometryList& out) override {
        try {
            runEngine(scene, out);
        }
        catch (const std::exception& e) {
            error("MeshSimplify failed: %s", e.what());
        }
        catch (...) {
            error("MeshSimplify failed: unknown exception");
        }
    }

    static const Description description;

private:
    float k_target_ratio;
    float k_target_error;
    float k_uv_weight;
    bool  k_lock_borders;
    bool  k_aggressive;
    bool  k_ignore_uv_seams;
    bool  k_prune_components;
    bool  k_output_quads;
    float k_quad_angle;
    bool  k_pin_silhouette;
    float k_silhouette_angle;

    bool  k_crop_box_on;
    bool  k_crop_cam_on;
    int   k_crop_mode;
    float k_crop_box[6];
    float k_overscan;
    bool  k_clip_near_far;
    int   k_frame_mode;
    bool  k_use_script_range;
    int   k_first_frame;
    int   k_last_frame;
    int   k_frame_step;

    // Resolved crop regions (world space), rebuilt in get_geometry_hash() on
    // the validating thread and read by geometry_engine().
    std::vector<Volume> boxes_;     // keep when inside ANY box (if have_box_)
    std::vector<Volume> frusta_;    // keep when inside ANY frustum (if have_cam_)
    bool have_box_ = false;
    bool have_cam_ = false;
    bool crop_active_ = false;
    std::string crop_status_;

    // -----------------------------------------------------------------------
    // Crop setup
    // -----------------------------------------------------------------------
    void build_crop() {
        boxes_.clear();
        frusta_.clear();
        crop_status_.clear();
        have_box_ = have_cam_ = false;

        if (k_crop_box_on) {
            have_box_ = true;
            build_boxes();
        }
        if (k_crop_cam_on && Op::input(1)) {
            build_frusta();
            // An unusable camera (orthographic, zero focal, ...) must not
            // silently delete everything: skip the camera crop and warn.
            have_cam_ = !frusta_.empty();
            if (!have_cam_) crop_status_ += " -- camera crop skipped";
        }
        crop_active_ = have_box_ || have_cam_;
    }

    void build_boxes() {
        Op* src = Op::input(2);
        GeoOp* geo = dynamic_cast<GeoOp*>(src);
        AxisOp* axis = geo ? nullptr : dynamic_cast<AxisOp*>(src);

        if (geo) {
            geo->validate(true);
            Scene s;
            geo->build_scene(s);
            const GeometryList* gl = s.object_list();
            const unsigned n = gl ? gl->objects() : 0;
            for (unsigned o = 0; o < n; ++o) {
                const GeoInfo& gi = (*gl)[o];
                const PointList* pl = gi.point_list();
                if (!pl || pl->size() == 0) continue;
                double lo[3] = {  1e300,  1e300,  1e300 };
                double hi[3] = { -1e300, -1e300, -1e300 };
                for (unsigned i = 0; i < pl->size(); ++i) {
                    const Vector3& p = (*pl)[i];
                    lo[0] = std::min(lo[0], double(p.x)); hi[0] = std::max(hi[0], double(p.x));
                    lo[1] = std::min(lo[1], double(p.y)); hi[1] = std::max(hi[1], double(p.y));
                    lo[2] = std::min(lo[2], double(p.z)); hi[2] = std::max(hi[2], double(p.z));
                }
                Volume v = box_volume(lo, hi, m4_from_float(gi.matrix.array()));
                if (!v.planes.empty()) boxes_.push_back(v);
            }
            if (boxes_.empty()) crop_status_ = "crop input has no points; box crop removes everything";
            return;
        }

        const double lo[3] = { std::min(k_crop_box[0], k_crop_box[3]),
                               std::min(k_crop_box[1], k_crop_box[4]),
                               std::min(k_crop_box[2], k_crop_box[5]) };
        const double hi[3] = { std::max(k_crop_box[0], k_crop_box[3]),
                               std::max(k_crop_box[1], k_crop_box[4]),
                               std::max(k_crop_box[2], k_crop_box[5]) };
        M4 xf;
        if (axis) {
            axis->validate(true);
            xf = m4_from_double(axis->worldTransform().array());
        } else {
            for (int i = 0; i < 16; ++i) xf.m[i] = (i % 5 == 0) ? 1.0 : 0.0;
        }
        Volume v = box_volume(lo, hi, xf);
        if (v.planes.empty()) crop_status_ = "crop axis transform is singular";
        else boxes_.push_back(v);
    }

    void build_frusta() {
        std::vector<double> frames;
        const double now = outputContext().frame();
        if (k_frame_mode == FRAMES_RANGE) {
            double first = k_first_frame, last = k_last_frame;
            if (k_use_script_range) {
                Op* root = rootOp();
                first = knob_at(root, "first_frame", now, 0, first);
                last  = knob_at(root, "last_frame", now, 0, last);
            }
            if (last < first) std::swap(first, last);
            const int step = std::max(1, k_frame_step);
            const int n = int(std::floor((last - first) / step)) + 1;
            const int kMaxSamples = 100000;
            for (int i = 0; i < n && i < kMaxSamples; ++i) frames.push_back(first + double(i) * step);
            if (frames.empty() || frames.back() < last) frames.push_back(last);
        } else {
            frames.push_back(now);
        }

        const Format& fmt = input_format();
        for (double t : frames) {
            CameraOp* cam = nullptr;
            if (k_frame_mode == FRAMES_CURRENT) {
                cam = dynamic_cast<CameraOp*>(Op::input(1));
            } else {
                OutputContext oc = outputContext();
                oc.setFrame(t);
                cam = dynamic_cast<CameraOp*>(node_input(1, Op::EXECUTABLE_INPUT, &oc));
            }
            if (!cam) { crop_status_ = "cam input is not a classic camera"; continue; }
            cam->validate(true);
            Volume v;
            std::string err;
            if (!frustum_volume(cam, t, fmt, v, err)) {
                crop_status_ = err;
                continue;
            }
            // A static camera yields identical frusta: keep one.
            if (frusta_.empty() || !same_volume(frusta_.back(), v)) frusta_.push_back(v);
        }
        if (frusta_.empty() && crop_status_.empty())
            crop_status_ = "no camera frustum could be built";
    }

    // World-space frustum of a perspective camera. Lens values are read from
    // knobs at frame t (member fields moved between Nuke 14 and 16); the
    // screen-window mapping is the one ScreenspaceToTracker fitted against
    // Reconcile3D: screen = R(roll) * S^-1 * (ndc - T), ndc = lens * xy / -z.
    bool frustum_volume(CameraOp* cam, double t, const Format& fmt, Volume& vol, std::string& err) {
        const double focal = knob_at(cam, "focal", t, 0, 50.0);
        const double hap   = knob_at(cam, "haperture", t, 0, 24.576);
        const double wtx   = knob_at(cam, "win_translate", t, 0, 0.0);
        const double wty   = knob_at(cam, "win_translate", t, 1, 0.0);
        const double wsx   = knob_at(cam, "win_scale", t, 0, 1.0);
        const double wsy   = knob_at(cam, "win_scale", t, 1, 1.0);
        const double roll  = knob_at(cam, "winroll", t, 0, 0.0);
        const double znear = knob_at(cam, "near", t, 0, 0.1);
        const double zfar  = knob_at(cam, "far", t, 0, 10000.0);
        const int projMode = int(knob_at(cam, "projection_mode", t, 0, 0.0));
        if (projMode != 0) { err = "camera crop supports perspective cameras only"; return false; }
        if (!(hap > 0.0) || !(focal > 0.0) || wsx == 0.0 || wsy == 0.0) {
            err = "camera has invalid focal / aperture / window scale";
            return false;
        }
        const double lens = 2.0 * focal / hap;

        // Pixel -> normalised screen: invert Nuke's own to_format mapping
        // (affine in x and y, sampled at three points).
        Matrix4 toFmt;
        toFmt.makeIdentity();
        CameraOp::to_format(toFmt, &fmt);
        const M4 F = m4_from_float(toFmt.array());
        const double ox = F.at(0, 3), oy = F.at(1, 3);
        const double ax = F.at(0, 0), bx = F.at(0, 1);
        const double ay = F.at(1, 0), by = F.at(1, 1);
        const double det = ax * by - bx * ay;
        if (std::fabs(det) < 1e-12) { err = "script format is degenerate"; return false; }

        const double os = std::max(0.0, double(k_overscan));
        const double px[4] = { fmt.x() - os, fmt.r() + os, fmt.r() + os, fmt.x() - os };
        const double py[4] = { fmt.y() - os, fmt.y() - os, fmt.t() + os, fmt.t() + os };
        const double ang = -roll * M_PI / 180.0;
        const double ca = std::cos(ang), sa = std::sin(ang);

        double dir[4][3];
        for (int i = 0; i < 4; ++i) {
            const double qx = px[i] - ox, qy = py[i] - oy;
            const double sx = ( by * qx - bx * qy) / det;
            const double sy = (-ay * qx + ax * qy) / det;
            // ndc = T + S * R(-roll) * screen
            const double rx = ca * sx - sa * sy;
            const double ry = sa * sx + ca * sy;
            const double nx = wtx + wsx * rx;
            const double ny = wty + wsy * ry;
            dir[i][0] = nx / lens;
            dir[i][1] = ny / lens;
            dir[i][2] = -1.0;
        }

        // Side planes through the eye: n = d_i x d_{i+1}, oriented so the
        // window's centre ray is inside. (Not the view axis: a shifted window
        // -- win_translate -- can put (0, 0, -1) on or outside a side plane.)
        double ctr[3] = { 0.0, 0.0, 0.0 };
        for (int i = 0; i < 4; ++i)
            for (int k = 0; k < 3; ++k) ctr[k] += 0.25 * dir[i][k];
        std::vector<Plane> cam_planes;
        for (int i = 0; i < 4; ++i) {
            const double* a = dir[i];
            const double* b = dir[(i + 1) % 4];
            double n[3] = { a[1] * b[2] - a[2] * b[1],
                            a[2] * b[0] - a[0] * b[2],
                            a[0] * b[1] - a[1] * b[0] };
            if (n[0] * ctr[0] + n[1] * ctr[1] + n[2] * ctr[2] < 0.0) {
                n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];
            }
            cam_planes.push_back(make_plane(n[0], n[1], n[2], 0.0));
        }
        if (k_clip_near_far) {
            cam_planes.push_back(make_plane(0, 0, -1, -std::max(0.0, znear)));  // -z >= near
            if (zfar > znear) cam_planes.push_back(make_plane(0, 0, 1, zfar));  // -z <= far
        } else {
            cam_planes.push_back(make_plane(0, 0, -1, 0.0));                    // in front
        }

        M4 W = m4_from_double(cam->worldTransform().array());
        M4 Winv;
        if (!m4_inverse(W, Winv)) { err = "camera transform is singular"; return false; }
        vol.planes.clear();
        for (const Plane& p : cam_planes) vol.planes.push_back(plane_through(p, Winv));
        return true;
    }

    // Per-face keep flags for one object. A face is in a volume unless (touching)
    // all its corners are outside one same plane, or (inside) any corner is
    // outside any plane. Kept = inside ANY box (if boxes are on) AND inside ANY
    // frustum (if the camera is on).
    void crop_faces(const PointList& pts, const Matrix4& obj_matrix,
                    const std::vector<uint32_t>& face_start,
                    const std::vector<uint32_t>& corner_pt,
                    std::vector<uint8_t>& keep) const
    {
        const size_t num_faces = face_start.size() - 1;
        keep.assign(num_faces, 1);
        const M4 M = m4_from_float(obj_matrix.array());
        const unsigned np = static_cast<unsigned>(pts.size());
        std::vector<uint16_t> mask(np);
        std::vector<uint8_t> hit;

        auto pass_set = [&](const std::vector<Volume>& vols) {
            hit.assign(num_faces, 0);
            for (const Volume& vol : vols) {
                const size_t npl = std::min<size_t>(vol.planes.size(), 16);
                Plane local[16];
                for (size_t i = 0; i < npl; ++i) local[i] = plane_through(vol.planes[i], M);
                for (unsigned i = 0; i < np; ++i) {
                    const Vector3& p = pts[i];
                    uint16_t m = 0;
                    for (size_t k = 0; k < npl; ++k) {
                        const double* q = local[k].v;
                        if (q[0] * p.x + q[1] * p.y + q[2] * p.z + q[3] < 0.0) m |= uint16_t(1u << k);
                    }
                    mask[i] = m;
                }
                for (size_t f = 0; f < num_faces; ++f) {
                    if (hit[f] || !keep[f]) continue;
                    const uint32_t b = face_start[f], e = face_start[f + 1];
                    if (k_crop_mode == CROP_INSIDE) {
                        uint16_t any = 0;
                        for (uint32_t c = b; c < e; ++c) any |= mask[corner_pt[c]];
                        if (any == 0) hit[f] = 1;
                    } else {
                        uint16_t all = 0xFFFF;
                        for (uint32_t c = b; c < e; ++c) all &= mask[corner_pt[c]];
                        if (all == 0) hit[f] = 1;
                    }
                }
            }
            for (size_t f = 0; f < num_faces; ++f) keep[f] = keep[f] && hit[f];
        };

        if (have_box_) pass_set(boxes_);
        if (have_cam_) pass_set(frusta_);
    }

    // -----------------------------------------------------------------------
    void runEngine(Scene& scene, GeometryList& out) {
        GeoOp* in = dynamic_cast<GeoOp*>(input0());
        if (!in) return;
        // 'out' is a cache that still holds THIS op's previous (rebuilt)
        // result. An unchanged input only refreshes the groups flagged for
        // rebuild and appends primitives to the objects already there, so
        // after a knob change here the input's faces pile up on top of the
        // stale output (measured: 400 -> 624 -> 800 faces). Start from an
        // empty list with every group flagged so the input tree rebuilds it
        // from scratch.
        out.delete_objects();
        out.set_rebuild(Mask_All_Geometry);
        in->get_geometry(scene, out);

        if (!crop_status_.empty()) warning("MeshSimplify crop: %s", crop_status_.c_str());

        const bool simplify = k_target_ratio < 0.9999f;
        if (!simplify && !k_output_quads && !crop_active_) return;

        const unsigned num_objects = out.objects();
        if (num_objects == 0) return;

        // Snapshot results and object state FIRST: after delete_objects() the
        // input GeoInfos are gone (NDK_NOTES 3.2).
        std::vector<OutMesh>  results(num_objects);
        std::vector<ObjState> state(num_objects);
        size_t faces_in = 0, faces_kept = 0;
        bool any_collapsed = false;
        for (unsigned obj = 0; obj < num_objects; ++obj) {
            const GeoInfo& gi = out[obj];
            state[obj].matrix      = gi.matrix;
            state[obj].material    = gi.material;
            state[obj].render_mode = gi.render_mode;
            state[obj].display3d   = gi.display3d;
            state[obj].selectable  = gi.selectable;
            results[obj] = processObject(gi, simplify);
            faces_in   += results[obj].faces_in;
            faces_kept += results[obj].faces_kept;
            any_collapsed = any_collapsed || results[obj].collapsed;
        }

        out.delete_objects();

        // Every object is re-added even when cropped to nothing: an EMPTY
        // GeometryList crashes Nuke 14.1's viewer (NDK_NOTES 46).
        for (unsigned obj = 0; obj < num_objects; ++obj) {
            const OutMesh& r = results[obj];
            out.add_object(obj);
            GeoInfo& gi = out[obj];
            gi.matrix      = state[obj].matrix;
            gi.material    = state[obj].material;
            gi.render_mode = state[obj].render_mode;
            gi.display3d   = state[obj].display3d;
            gi.selectable  = state[obj].selectable;

            if (!r.valid || r.points.empty() ||
                (r.tri_indices.empty() && r.poly_sizes.empty()))
            {
                continue;
            }

            const size_t num_tris = r.tri_indices.size() / 3;
            for (size_t t = 0; t < num_tris; ++t) {
                out.add_primitive(obj, new Triangle(
                    r.tri_indices[t * 3 + 0],
                    r.tri_indices[t * 3 + 1],
                    r.tri_indices[t * 3 + 2]));
            }

            // Fully qualified: wingdi.h declares a global Polygon() (NDK_NOTES 2.2).
            size_t c = 0;
            for (uint32_t n : r.poly_sizes) {
                DD::Image::Polygon* poly = new DD::Image::Polygon(/*vertices=*/n, /*closed=*/true);
                for (uint32_t k = 0; k < n; ++k) poly->vertex(k) = r.poly_indices[c + k];
                c += n;
                out.add_primitive(obj, poly);
            }

            PointList* pl = out.writable_points(obj);
            pl->resize(static_cast<unsigned>(r.points.size()));
            for (size_t i = 0; i < r.points.size(); ++i)
                (*pl)[static_cast<unsigned>(i)] = r.points[i];

            if (r.has_uv) {
                Attribute* uv_attr = out.writable_attribute(obj, Group_Vertices, "uv", VECTOR4_ATTRIB);
                if (uv_attr) {
                    const size_t total = r.tri_uvs.size() + r.poly_uvs.size();
                    uv_attr->resize(static_cast<unsigned>(total));
                    unsigned idx = 0;
                    for (const Vector4& uv : r.tri_uvs)  uv_attr->vector4(idx++) = uv;
                    for (const Vector4& uv : r.poly_uvs) uv_attr->vector4(idx++) = uv;
                }
            }
        }

        if (crop_active_ && faces_in > 0 && faces_kept == 0)
            warning("MeshSimplify: the crop removed every face.");
        else if (any_collapsed)
            warning("MeshSimplify: an object collapsed to zero triangles. "
                    "Raise target_ratio or lower max_error.");
    }

    // -----------------------------------------------------------------------
    OutMesh processObject(const GeoInfo& info, bool simplify) {
        OutMesh result;

        const PointList* points_in = info.point_list();
        if (!points_in || points_in->size() < 3) return result;
        const unsigned num_points = static_cast<unsigned>(points_in->size());

        const unsigned num_prims = info.primitives();
        if (num_prims == 0) return result;
        const Primitive** prims = info.primitive_array();
        if (!prims) return result;

        // uv may live on the vertex group (per corner) OR the point group: the
        // Cube of Nuke 15.1 - 16.0 emits point uvs. get_typed_attribute()
        // returns either, so look the group up explicitly -- indexing a point
        // attribute by vertex slot reads past its end (random crashes).
        // Vector4 (Nuke's native) before Vector2 (some readers).
        const Attribute* uv_attr = nullptr;
        bool uv_is_v2 = false, uv_per_point = false;
        {
            const int groups[2] = { Group_Vertices, Group_Points };
            for (int g = 0; g < 2 && !uv_attr; ++g) {
                if ((uv_attr = info.get_typed_group_attribute(groups[g], "uv", VECTOR4_ATTRIB))) {
                    uv_is_v2 = false;
                } else if ((uv_attr = info.get_typed_group_attribute(groups[g], "uv", VECTOR2_ATTRIB))) {
                    uv_is_v2 = true;
                }
                uv_per_point = uv_attr && groups[g] == Group_Points;
            }
        }
        const unsigned uv_size = uv_attr ? uv_attr->size() : 0u;
        result.has_uv = (uv_attr != nullptr && uv_size > 0);

        // 1) Faces as flat corner lists (point index + raw uv per corner).
        std::vector<uint32_t> face_start;
        std::vector<uint32_t> corner_pt;
        std::vector<Vector4>  corner_uv;
        face_start.reserve(static_cast<size_t>(num_prims) + 1);
        corner_pt.reserve(static_cast<size_t>(num_prims) * 4);
        if (result.has_uv) corner_uv.reserve(static_cast<size_t>(num_prims) * 4);
        face_start.push_back(0);

        std::vector<unsigned> face_local;
        for (unsigned p = 0; p < num_prims; ++p) {
            const Primitive* prim = prims[p];
            if (!prim) continue;
            const unsigned face_count = prim->faces();
            const unsigned voff = prim->vertex_offset();
            for (unsigned f = 0; f < face_count; ++f) {
                const unsigned fv = prim->face_vertices(f);
                if (fv < 3) continue;
                face_local.resize(fv);
                prim->get_face_vertices(f, face_local.data());
                for (unsigned k = 0; k < fv; ++k) {
                    const unsigned lv = face_local[k];
                    const unsigned pi = prim->vertex(lv);
                    corner_pt.push_back(pi < num_points ? pi : 0);
                    if (result.has_uv) {
                        const unsigned slot = uv_per_point ? pi : voff + lv;
                        if (slot >= uv_size) {
                            corner_uv.push_back(Vector4(0.0f, 0.0f, 0.0f, 1.0f));
                        } else if (uv_is_v2) {
                            const Vector2& uv = uv_attr->vector2(slot);
                            corner_uv.push_back(Vector4(uv.x, uv.y, 0.0f, 1.0f));
                        } else {
                            corner_uv.push_back(uv_attr->vector4(slot));
                        }
                    }
                }
                face_start.push_back(static_cast<uint32_t>(corner_pt.size()));
            }
        }
        const size_t num_faces = face_start.size() - 1;
        result.faces_in = num_faces;
        if (num_faces == 0) return result;

        // 2) Crop.
        std::vector<uint8_t> keep;
        if (crop_active_) crop_faces(*points_in, info.matrix, face_start, corner_pt, keep);
        else keep.assign(num_faces, 1);
        for (uint8_t k : keep) result.faces_kept += k;
        if (result.faces_kept == 0) { result.valid = true; return result; }

        // 3a) Crop only: pass kept faces through with their own layout.
        if (!simplify && !k_output_quads) {
            std::vector<uint32_t> remap(num_points, UINT32_MAX);
            for (size_t f = 0; f < num_faces; ++f) {
                if (!keep[f]) continue;
                const uint32_t b = face_start[f], e = face_start[f + 1];
                for (uint32_t c = b; c < e; ++c) {
                    uint32_t& slot = remap[corner_pt[c]];
                    if (slot == UINT32_MAX) {
                        slot = static_cast<uint32_t>(result.points.size());
                        result.points.push_back((*points_in)[corner_pt[c]]);
                    }
                }
                if (e - b == 3) {
                    for (uint32_t c = b; c < e; ++c) {
                        result.tri_indices.push_back(remap[corner_pt[c]]);
                        if (result.has_uv) result.tri_uvs.push_back(corner_uv[c]);
                    }
                } else {
                    result.poly_sizes.push_back(e - b);
                    for (uint32_t c = b; c < e; ++c) {
                        result.poly_indices.push_back(remap[corner_pt[c]]);
                        if (result.has_uv) result.poly_uvs.push_back(corner_uv[c]);
                    }
                }
            }
            result.valid = true;
            return result;
        }

        // 3b) Fan-triangulate kept faces into a corner stream of
        // (point, u, v) with homogeneous uv unprojected.
        struct Corner { uint32_t p; float u, v; };
        std::vector<Corner> corners;
        corners.reserve(corner_pt.size() * 3 / 2);
        auto mk = [&](uint32_t c) -> Corner {
            Corner k;
            k.p = corner_pt[c];
            if (result.has_uv) {
                const Vector4& uv = corner_uv[c];
                const float invq = (uv.w != 0.0f) ? (1.0f / uv.w) : 1.0f;
                k.u = uv.x * invq;
                k.v = uv.y * invq;
            } else {
                k.u = k.v = 0.0f;
            }
            return k;
        };
        for (size_t f = 0; f < num_faces; ++f) {
            if (!keep[f]) continue;
            const uint32_t b = face_start[f], e = face_start[f + 1];
            const Corner c0 = mk(b);
            for (uint32_t k = b + 1; k + 1 < e; ++k) {
                corners.push_back(c0);
                corners.push_back(mk(k));
                corners.push_back(mk(k + 1));
            }
        }
        if (corners.size() < 3) { result.valid = true; return result; }

        // 4) Fat vertices: unique (point, u, v) so UV seams become topology
        // breaks; with ignore_uv_seams the key is the point alone (first-seen
        // UV wins). meshopt_generateVertexRemap hashes the bytes of each key.
        const size_t num_indices = corners.size();
        std::vector<uint32_t> indices(num_indices);
        size_t num_verts = 0;
        if (k_ignore_uv_seams) {
            std::vector<uint32_t> keys(num_indices);
            for (size_t i = 0; i < num_indices; ++i) keys[i] = corners[i].p;
            num_verts = meshopt_generateVertexRemap(indices.data(), nullptr, num_indices,
                                                    keys.data(), num_indices, sizeof(uint32_t));
        } else {
            static_assert(sizeof(Corner) == 12, "Corner must be tightly packed");
            num_verts = meshopt_generateVertexRemap(indices.data(), nullptr, num_indices,
                                                    corners.data(), num_indices, sizeof(Corner));
        }
        struct FatVertex { float x, y, z, u, v; uint32_t src; };
        std::vector<FatVertex> fat(num_verts);
        std::vector<uint8_t> seen(num_verts, 0);
        for (size_t i = 0; i < num_indices; ++i) {
            const uint32_t id = indices[i];
            if (seen[id]) continue;
            seen[id] = 1;
            const Vector3& P = (*points_in)[corners[i].p];
            fat[id] = FatVertex{ P.x, P.y, P.z, corners[i].u, corners[i].v, corners[i].p };
        }
        std::vector<Corner>().swap(corners);

        // 4b) pin_silhouette: lock both ends of every interior edge whose
        // dihedral angle exceeds the threshold.
        std::vector<unsigned char> vertex_lock;
        if (k_pin_silhouette && !k_aggressive && simplify) {
            const size_t nt = num_indices / 3;
            std::vector<Vector3> nrm(nt);
            for (size_t t = 0; t < nt; ++t) {
                const FatVertex& a = fat[indices[t * 3 + 0]];
                const FatVertex& b = fat[indices[t * 3 + 1]];
                const FatVertex& c = fat[indices[t * 3 + 2]];
                nrm[t] = tri_normal(Vector3(a.x, a.y, a.z), Vector3(b.x, b.y, b.z), Vector3(c.x, c.y, c.z));
            }
            std::vector<EdgeEntry> edges;
            build_sorted_edges(indices.data(), nt, edges);
            vertex_lock.assign(num_verts, 0);
            const float cos_threshold = std::cos(k_silhouette_angle * float(M_PI / 180.0));
            for_each_manifold_edge(edges, [&](const EdgeEntry& e1, const EdgeEntry& e2) {
                if (nrm[e1.tri].dot(nrm[e2.tri]) < cos_threshold) {
                    vertex_lock[static_cast<uint32_t>(e1.key >> 32)] = 1;
                    vertex_lock[static_cast<uint32_t>(e1.key & 0xFFFFFFFFull)] = 1;
                }
            });
        }

        // 5) Simplify.
        std::vector<uint32_t> out_indices;
        if (simplify) {
            size_t target_idx = static_cast<size_t>(double(num_indices) * double(k_target_ratio));
            target_idx -= (target_idx % 3);
            if (target_idx < 3) target_idx = 3;

            out_indices.assign(num_indices, 0);
            const float* pos_base = &fat[0].x;
            const float* uv_base  = &fat[0].u;
            const size_t stride = sizeof(FatVertex);
            const float attr_weights[2] = { k_uv_weight, k_uv_weight };

            unsigned int options = 0;
            if (k_lock_borders)     options |= meshopt_SimplifyLockBorder;
            if (k_prune_components) options |= meshopt_SimplifyPrune;

            float result_error = 0.0f;
            size_t count = 0;
            if (k_aggressive) {
                count = meshopt_simplifySloppy(out_indices.data(), indices.data(), num_indices,
                                               pos_base, num_verts, stride,
                                               target_idx, k_target_error, &result_error);
            } else {
                count = meshopt_simplifyWithAttributes(out_indices.data(), indices.data(), num_indices,
                                                       pos_base, num_verts, stride,
                                                       uv_base, stride, attr_weights, 2,
                                                       vertex_lock.empty() ? nullptr : vertex_lock.data(),
                                                       target_idx, k_target_error, options, &result_error);
            }
            if (count < 3) { result.collapsed = true; return result; }
            out_indices.resize(count);
        } else {
            out_indices.swap(indices);
        }

        // 6) Back to Nuke points: one point per surviving source point (UV-seam
        // siblings share it); per-corner UVs carry the seam.
        std::vector<uint32_t> src_to_point(num_points, UINT32_MAX);
        const size_t num_new_tris = out_indices.size() / 3;
        result.tri_indices.resize(num_new_tris * 3);
        if (result.has_uv) result.tri_uvs.resize(num_new_tris * 3);
        for (size_t i = 0; i < out_indices.size(); ++i) {
            const FatVertex& fv = fat[out_indices[i]];
            uint32_t& slot = src_to_point[fv.src];
            if (slot == UINT32_MAX) {
                slot = static_cast<uint32_t>(result.points.size());
                result.points.emplace_back(fv.x, fv.y, fv.z);
            }
            result.tri_indices[i] = slot;
            if (result.has_uv) result.tri_uvs[i] = Vector4(fv.u, fv.v, 0.0f, 1.0f);
        }

        // 7) Optional quad merge.
        if (k_output_quads && num_new_tris >= 2) {
            mergeCoplanarTriPairs(result.tri_indices, result.tri_uvs,
                                  result.poly_sizes, result.poly_indices, result.poly_uvs,
                                  result.points, k_quad_angle * float(M_PI / 180.0),
                                  result.has_uv);
        }

        result.valid = true;
        return result;
    }
};

// ---------------------------------------------------------------------------

static Op* build(Node* node) { return new MeshSimplify(node); }

const Op::Description MeshSimplify::description(kClass, "Geometry/MeshSimplify", build);

} // anonymous namespace
