// NukeMeshSimplify.cpp
//
// Nuke 14 NDK plugin. Simplify (meshoptimizer, MIT) plus an optional
// post-process that merges coplanar triangle pairs into quads.
//
// Key knobs:
//   target_ratio, target_error, uv_weight   -- core simplifier controls
//   ignore_uv_seams                         -- bypass UV-seam preservation
//                                              for more aggressive collapse
//                                              of flat areas (e.g. cube faces)
//   prune_components                        -- drop small disconnected pieces
//   output_quads, quad_angle                -- merge coplanar tri pairs into
//                                              Polygon quads, dihedral angle
//                                              under quad_angle (degrees)

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
#include "DDImage/Vector4.h"
#include "DDImage/Vector3.h"
#include "DDImage/Attribute.h"
#include "DDImage/Hash.h"

#include <meshoptimizer.h>

#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <exception>

using namespace DD::Image;

namespace {

static const char* const kClass = "MeshSimplify";

static const char* const kHelp =
    "<b>MeshSimplify</b><br>"
    "<i>UV-aware mesh decimation and optional quad-pair merging for Nuke 14.</i><br>"
    "Created by Marten Blumen.<br>"
    "<br>"
    "<b>Algorithm</b><br>"
    "Quadric edge-collapse simplification via meshoptimizer (MIT). A fat-vertex bridge<br>"
    "keys unique (point, UV) pairs so texture seams become topological boundaries<br>"
    "the simplifier preserves automatically. Optional feature-edge detection locks vertices on<br>"
    "sharp creases by passing a vertex_lock array to meshopt_simplifyWithAttributes. After<br>"
    "simplification, an optional coplanar triangle-pair merger promotes flat areas to quad<br>"
    "Polygon primitives, only crossing edges where UV continuity holds across both sides.<br>"
    "<br>"
    "<b>Usage notes</b><br>"
    "For open meshes (a plane with a hole) use 'lock borders' to<br>"
    "keep the boundary loop intact. For closed meshes like a cube, 'lock<br>"
    "borders' has no effect since every edge has two faces; use 'pin<br>"
    "silhouette' instead to preserve sharp corners via dihedral angle detection. The<br>"
    "'output quads' option is a pair-merger, not a regular quad remesher --<br>"
    "it works perfectly for flat surfaces but will not produce regular quad<br>"
    "topology on curved meshes; those require a dedicated remesher such as<br>"
    "Instant Meshes or libQEx. The 'aggressive' path is faster but ignores<br>"
    "both UV awareness and the silhouette pin.";

// ---------------------------------------------------------------------------
// Fat-vertex bridge (unchanged design: unique (pointIdx, u, v) tuples).
// With ignore_uv_seams=true the key collapses to pointIdx alone, dropping
// UV-driven topological discontinuities so the simplifier sees a more
// connected mesh.
// ---------------------------------------------------------------------------

struct FatKey {
    uint32_t pointIdx;
    float    u, v;
    bool operator==(const FatKey& o) const noexcept {
        return pointIdx == o.pointIdx
            && std::memcmp(&u, &o.u, sizeof(float)) == 0
            && std::memcmp(&v, &o.v, sizeof(float)) == 0;
    }
};

struct FatKeyHash {
    size_t operator()(const FatKey& k) const noexcept {
        uint32_t ub = 0, vb = 0;
        std::memcpy(&ub, &k.u, sizeof(float));
        std::memcpy(&vb, &k.v, sizeof(float));
        uint64_t h = static_cast<uint64_t>(k.pointIdx) * 0x9e3779b97f4a7c15ull;
        h ^= (static_cast<uint64_t>(ub) + 0x517cc1b727220a95ull);
        h *= 0x9e3779b97f4a7c15ull;
        h ^= (static_cast<uint64_t>(vb) + 0x6a09e667f3bcc908ull);
        h ^= (h >> 32);
        return static_cast<size_t>(h);
    }
};

struct FatVertex {
    float    x, y, z;
    float    u, v;
    uint32_t srcPointIdx;
};

// Output of simplifyObject(): triangles AND quads coexist; quads come from
// the optional post-process. Write-back emits them in this order: all
// triangles first, then all quads. UVs follow the same order.
struct SimplifiedMesh {
    std::vector<Vector3>  points;
    std::vector<uint32_t> tri_indices;    // 3 per tri
    std::vector<uint32_t> quad_indices;   // 4 per quad
    std::vector<Vector4>  tri_uvs;        // 3 per tri (per-corner)
    std::vector<Vector4>  quad_uvs;       // 4 per quad (per-corner)
    bool has_uv = false;
    bool valid  = false;
};

// ---------------------------------------------------------------------------
// Quad-pair merger: take a triangle list, merge pairs that are coplanar
// (within angle_threshold_rad) and have UV continuity across the shared
// edge.
//
// Algorithm: build edge -> triangles map, scan candidates, sort by
// dihedral angle (most planar first), greedy match. Output unmerged
// triangles and merged quads. Quad vertex order is CCW: [opp_t1, shared_a,
// opp_t2, shared_b] which preserves T1's normal direction.
// ---------------------------------------------------------------------------
static void mergeCoplanarTriPairs(
    std::vector<uint32_t>& tri_indices,    // in/out: shrinks
    std::vector<Vector4>&  tri_uvs,        // in/out: shrinks
    std::vector<uint32_t>& quad_indices,   // out
    std::vector<Vector4>&  quad_uvs,       // out
    const std::vector<Vector3>& points,
    float angle_threshold_rad,
    bool  has_uv)
{
    const size_t num_tris = tri_indices.size() / 3;
    if (num_tris < 2) return;

    // 1) Face normals.
    std::vector<Vector3> normals(num_tris);
    for (size_t t = 0; t < num_tris; ++t) {
        const Vector3& a = points[tri_indices[t * 3 + 0]];
        const Vector3& b = points[tri_indices[t * 3 + 1]];
        const Vector3& c = points[tri_indices[t * 3 + 2]];
        Vector3 n = (b - a).cross(c - a);
        const float len = n.length();
        if (len > 0.0f) n /= len;
        normals[t] = n;
    }

    // 2) Edge -> [(triangle, local edge slot)] map. Edge key is the
    // unordered point-index pair packed into a uint64.
    struct EdgeRef { uint32_t tri; uint8_t edge; };
    auto edge_key = [](uint32_t a, uint32_t b) -> uint64_t {
        if (a > b) std::swap(a, b);
        return (static_cast<uint64_t>(a) << 32) | static_cast<uint64_t>(b);
    };

    std::unordered_map<uint64_t, std::vector<EdgeRef>> edge_map;
    edge_map.reserve(num_tris * 3);

    for (uint32_t t = 0; t < num_tris; ++t) {
        for (uint8_t e = 0; e < 3; ++e) {
            uint32_t va = tri_indices[t * 3 + e];
            uint32_t vb = tri_indices[t * 3 + ((e + 1) % 3)];
            EdgeRef ref{ t, e };
            edge_map[edge_key(va, vb)].push_back(ref);
        }
    }

    // 3) Candidate list: edges shared by exactly 2 triangles with small
    // dihedral angle and matching UVs at both shared vertices.
    struct Candidate {
        uint32_t t1, t2;
        uint8_t  e1, e2;
        float    dot;    // higher = more planar
    };
    std::vector<Candidate> candidates;
    const float cos_threshold = std::cos(angle_threshold_rad);

    auto uv_close = [](const Vector4& a, const Vector4& b) {
        // Compare just u,v -- ignore w (already unprojected at read time).
        const float du = a.x - b.x;
        const float dv = a.y - b.y;
        return (du * du + dv * dv) < 1.0e-10f;
    };

    for (auto& kv : edge_map) {
        if (kv.second.size() != 2) continue;
        const EdgeRef r1 = kv.second[0];
        const EdgeRef r2 = kv.second[1];

        const float d = normals[r1.tri].dot(normals[r2.tri]);
        if (d < cos_threshold) continue;   // too non-planar

        // UV-continuity check. Look up UV at each shared vertex from both
        // triangles' perspectives; if they differ, this edge is a UV seam
        // and merging would tear the texture.
        if (has_uv) {
            // T1: edge from local e1 to (e1+1)%3
            const Vector4& uv_t1_a = tri_uvs[r1.tri * 3 + r1.edge];
            const Vector4& uv_t1_b = tri_uvs[r1.tri * 3 + ((r1.edge + 1) % 3)];

            // T2's same edge runs in reverse (consistent winding):
            // its local e2 vertex == T1's vertex (e1+1)%3 == b
            // its (e2+1)%3 vertex == T1's vertex e1            == a
            const Vector4& uv_t2_b = tri_uvs[r2.tri * 3 + r2.edge];
            const Vector4& uv_t2_a = tri_uvs[r2.tri * 3 + ((r2.edge + 1) % 3)];

            if (!uv_close(uv_t1_a, uv_t2_a)) continue;
            if (!uv_close(uv_t1_b, uv_t2_b)) continue;
        }

        candidates.push_back({ r1.tri, r2.tri, r1.edge, r2.edge, d });
    }

    // 4) Greedy match: most planar first.
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) { return a.dot > b.dot; });

    std::vector<uint8_t> consumed(num_tris, 0);
    std::vector<uint32_t> new_quad_indices;
    std::vector<Vector4>  new_quad_uvs;
    new_quad_indices.reserve(candidates.size() * 4);
    if (has_uv) new_quad_uvs.reserve(candidates.size() * 4);

    for (const Candidate& c : candidates) {
        if (consumed[c.t1] || consumed[c.t2]) continue;

        // T1 = (v[e1], v[e1+1], v[e1+2]) where (v[e1], v[e1+1]) is the
        // shared edge. Call those (a, b); the opposite vertex is c.
        const uint32_t a   = tri_indices[c.t1 * 3 + c.e1];
        const uint32_t b   = tri_indices[c.t1 * 3 + ((c.e1 + 1) % 3)];
        const uint32_t op1 = tri_indices[c.t1 * 3 + ((c.e1 + 2) % 3)];

        // T2's opposite vertex (the one that is neither a nor b).
        uint32_t op2 = 0;
        for (int k = 0; k < 3; ++k) {
            const uint32_t v = tri_indices[c.t2 * 3 + k];
            if (v != a && v != b) { op2 = v; break; }
        }

        // CCW quad: [op1, a, op2, b] preserves T1's normal direction.
        // (Walking c -> a uses T1's edge c->a, a -> d uses T2's edge a->d,
        //  d -> b uses T2's edge d->b, b -> c uses T1's edge b->c.)
        new_quad_indices.push_back(op1);
        new_quad_indices.push_back(a);
        new_quad_indices.push_back(op2);
        new_quad_indices.push_back(b);

        if (has_uv) {
            // UVs at corresponding corners. T1 owns op1, a, b. T2 owns op2.
            const Vector4 uv_op1 = tri_uvs[c.t1 * 3 + ((c.e1 + 2) % 3)];
            const Vector4 uv_a   = tri_uvs[c.t1 * 3 + c.e1];
            const Vector4 uv_b   = tri_uvs[c.t1 * 3 + ((c.e1 + 1) % 3)];
            // For op2 we have to find its local slot in t2.
            Vector4 uv_op2(0.0f, 0.0f, 0.0f, 1.0f);
            for (int k = 0; k < 3; ++k) {
                const uint32_t v = tri_indices[c.t2 * 3 + k];
                if (v != a && v != b) {
                    uv_op2 = tri_uvs[c.t2 * 3 + k];
                    break;
                }
            }
            new_quad_uvs.push_back(uv_op1);
            new_quad_uvs.push_back(uv_a);
            new_quad_uvs.push_back(uv_op2);
            new_quad_uvs.push_back(uv_b);
        }

        consumed[c.t1] = 1;
        consumed[c.t2] = 1;
    }

    // 5) Repack: keep only unconsumed triangles.
    std::vector<uint32_t> new_tri_indices;
    std::vector<Vector4>  new_tri_uvs;
    new_tri_indices.reserve(tri_indices.size());
    if (has_uv) new_tri_uvs.reserve(tri_uvs.size());

    for (uint32_t t = 0; t < num_tris; ++t) {
        if (consumed[t]) continue;
        new_tri_indices.push_back(tri_indices[t * 3 + 0]);
        new_tri_indices.push_back(tri_indices[t * 3 + 1]);
        new_tri_indices.push_back(tri_indices[t * 3 + 2]);
        if (has_uv) {
            new_tri_uvs.push_back(tri_uvs[t * 3 + 0]);
            new_tri_uvs.push_back(tri_uvs[t * 3 + 1]);
            new_tri_uvs.push_back(tri_uvs[t * 3 + 2]);
        }
    }

    tri_indices.swap(new_tri_indices);
    tri_uvs.swap(new_tri_uvs);
    quad_indices.swap(new_quad_indices);
    quad_uvs.swap(new_quad_uvs);
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
    }

    const char* Class() const override { return kClass; }
    const char* node_help() const override { return kHelp; }

    int minimum_inputs() const override { return 1; }
    int maximum_inputs() const override { return 1; }

    void knobs(Knob_Callback f) override {
        GeoOp::knobs(f);

        Float_knob(f, &k_target_ratio, "target_ratio", "target ratio");
        SetRange(f, 0.0, 1.0);
        Tooltip(f, "Fraction of triangles to keep. 1.0 leaves the mesh "
                   "unchanged.");

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
        Tooltip(f, "Protect open mesh boundaries from collapse. (Has no "
                   "effect on closed meshes -- for closed shapes like a "
                   "cube, use 'pin silhouette' instead.)");

        Bool_knob(f, &k_pin_silhouette, "pin_silhouette", "pin silhouette");
        Tooltip(f, "Preserve the overall shape of the geometry by locking "
                   "vertices on feature edges (where adjacent faces meet "
                   "at a sharp angle). Use this to keep a cube cube-shaped, "
                   "a beveled edge sharp, etc. Not supported by the "
                   "aggressive simplifier.");

        Float_knob(f, &k_silhouette_angle, "silhouette_angle", "silhouette angle");
        SetRange(f, 0.0, 90.0);
        Tooltip(f, "Dihedral angle threshold in degrees. Edges where "
                   "adjacent faces meet at a wider angle than this are "
                   "treated as silhouette features and their vertices are "
                   "locked. 30 is a sensible default; lower values pin "
                   "more edges, higher values pin only the sharpest.");

        Bool_knob(f, &k_aggressive, "aggressive", "aggressive");
        Tooltip(f, "Sloppy simplifier: faster, ignores topology, lower "
                   "quality. UV-aware mode is disabled in this path.");

        Divider(f, "flat-area handling");

        Bool_knob(f, &k_ignore_uv_seams, "ignore_uv_seams", "ignore uv seams");
        Tooltip(f, "Treat the mesh as topologically connected regardless of "
                   "UV seams. Allows aggressive collapse of flat areas "
                   "split across per-face UV mappings (e.g. cube faces). "
                   "UV continuity across seams is no longer guaranteed.");

        Bool_knob(f, &k_prune_components, "prune_components", "prune small components");
        Tooltip(f, "Allow the simplifier to remove small disconnected mesh "
                   "components entirely instead of preserving them.");

        Divider(f, "quad merging");

        Bool_knob(f, &k_output_quads, "output_quads", "output quads");
        Tooltip(f, "After simplification, merge coplanar triangle pairs that "
                   "share an edge into Polygon quads. Only edges with "
                   "continuous UVs are merged. Note: this is a pair "
                   "merger, not a regular quad remesher -- it works well "
                   "for flat regions but won't produce regular quad "
                   "topology on curved surfaces.");

        Float_knob(f, &k_quad_angle, "quad_angle", "max quad angle");
        SetRange(f, 0.0, 45.0);
        Tooltip(f, "Maximum dihedral angle in degrees between two triangles "
                   "for them to be merged into a quad. 0 means perfectly "
                   "coplanar only; higher values allow gentle curvature.");
    }

    void get_geometry_hash() override {
        GeoOp::get_geometry_hash();

        geo_hash[Group_Points].append(k_target_ratio);
        geo_hash[Group_Points].append(k_target_error);
        geo_hash[Group_Points].append(k_uv_weight);
        geo_hash[Group_Points].append(k_lock_borders);
        geo_hash[Group_Points].append(k_aggressive);
        geo_hash[Group_Points].append(k_ignore_uv_seams);
        geo_hash[Group_Points].append(k_prune_components);
        geo_hash[Group_Points].append(k_output_quads);
        geo_hash[Group_Points].append(k_quad_angle);
        geo_hash[Group_Points].append(k_pin_silhouette);
        geo_hash[Group_Points].append(k_silhouette_angle);

        geo_hash[Group_Primitives].append(k_target_ratio);
        geo_hash[Group_Primitives].append(k_target_error);
        geo_hash[Group_Primitives].append(k_aggressive);
        geo_hash[Group_Primitives].append(k_ignore_uv_seams);
        geo_hash[Group_Primitives].append(k_prune_components);
        geo_hash[Group_Primitives].append(k_output_quads);
        geo_hash[Group_Primitives].append(k_quad_angle);
        geo_hash[Group_Primitives].append(k_pin_silhouette);
        geo_hash[Group_Primitives].append(k_silhouette_angle);

        geo_hash[Group_Attributes].append(k_target_ratio);
        geo_hash[Group_Attributes].append(k_uv_weight);
        geo_hash[Group_Attributes].append(k_ignore_uv_seams);
        geo_hash[Group_Attributes].append(k_output_quads);
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

    // -----------------------------------------------------------------------
    void runEngine(Scene& scene, GeometryList& out) {
        GeoOp* in = dynamic_cast<GeoOp*>(input0());
        if (!in) return;
        in->get_geometry(scene, out);

        if (k_target_ratio >= 0.9999f && !k_output_quads) return;

        const unsigned num_objects = out.objects();
        if (num_objects == 0) return;

        std::vector<SimplifiedMesh> results(num_objects);
        for (unsigned obj = 0; obj < num_objects; ++obj) {
            results[obj] = simplifyObject(out[obj]);
        }

        out.delete_objects();

        for (unsigned obj = 0; obj < num_objects; ++obj) {
            const SimplifiedMesh& r = results[obj];

            out.add_object(obj);

            if (!r.valid || r.points.empty() ||
                (r.tri_indices.empty() && r.quad_indices.empty()))
            {
                continue;
            }

            // Triangles first.
            const size_t num_tris = r.tri_indices.size() / 3;
            for (size_t t = 0; t < num_tris; ++t) {
                out.add_primitive(obj, new Triangle(
                    r.tri_indices[t * 3 + 0],
                    r.tri_indices[t * 3 + 1],
                    r.tri_indices[t * 3 + 2]
                ));
            }

            // Then quads as 4-vertex closed Polygons. Fully qualified
            // because wingdi.h declares a global Polygon() function
            // (GDI drawing primitive) that collides with the class name
            // even through 'using namespace DD::Image'.
            const size_t num_quads = r.quad_indices.size() / 4;
            for (size_t q = 0; q < num_quads; ++q) {
                DD::Image::Polygon* poly = new DD::Image::Polygon(/*vertices=*/4, /*closed=*/true);
                poly->vertex(0) = r.quad_indices[q * 4 + 0];
                poly->vertex(1) = r.quad_indices[q * 4 + 1];
                poly->vertex(2) = r.quad_indices[q * 4 + 2];
                poly->vertex(3) = r.quad_indices[q * 4 + 3];
                out.add_primitive(obj, poly);
            }

            // Points.
            PointList* pl = out.writable_points(obj);
            pl->resize(static_cast<unsigned>(r.points.size()));
            for (size_t i = 0; i < r.points.size(); ++i) {
                (*pl)[static_cast<unsigned>(i)] = r.points[i];
            }

            // UVs: per-corner, in the order primitives were added (all
            // triangle corners first, then all quad corners).
            if (r.has_uv) {
                Attribute* uv_attr = out.writable_attribute(
                    obj, Group_Vertices, "uv", VECTOR4_ATTRIB);
                if (uv_attr) {
                    const size_t total = r.tri_uvs.size() + r.quad_uvs.size();
                    uv_attr->resize(static_cast<unsigned>(total));
                    unsigned idx = 0;
                    for (const Vector4& uv : r.tri_uvs)  uv_attr->vector4(idx++) = uv;
                    for (const Vector4& uv : r.quad_uvs) uv_attr->vector4(idx++) = uv;
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    SimplifiedMesh simplifyObject(const GeoInfo& info) {
        SimplifiedMesh result;

        const PointList* points_in = info.point_list();
        if (!points_in || points_in->size() < 3) return result;

        const unsigned num_prims = info.primitives();
        if (num_prims == 0) return result;

        const Primitive** prims = info.primitive_array();
        if (!prims) return result;

        // Try Vector4 first (Nuke's native), then Vector2 (some readers).
        const Attribute* uv_attr = info.get_typed_attribute("uv", VECTOR4_ATTRIB);
        bool uv_is_v2 = false;
        if (!uv_attr) {
            uv_attr = info.get_typed_attribute("uv", VECTOR2_ATTRIB);
            uv_is_v2 = (uv_attr != nullptr);
        }
        result.has_uv = (uv_attr != nullptr);

        // 1) Triangulate input into a flat corner list.
        struct Corner {
            uint32_t pointIdx;
            float    u, v;
        };
        std::vector<Corner> corners;
        corners.reserve(static_cast<size_t>(num_prims) * 3);

        for (unsigned p = 0; p < num_prims; ++p) {
            const Primitive* prim = prims[p];
            if (!prim) continue;
            const unsigned face_count = prim->faces();
            if (face_count == 0) continue;

            for (unsigned f = 0; f < face_count; ++f) {
                const unsigned fv = prim->face_vertices(f);
                if (fv < 3) continue;

                std::vector<unsigned> face_local(fv);
                prim->get_face_vertices(f, face_local.data());

                auto resolve = [&](unsigned local_v) -> Corner {
                    Corner c;
                    c.pointIdx = static_cast<uint32_t>(prim->vertex(local_v));
                    if (result.has_uv) {
                        const unsigned slot = prim->vertex_offset() + local_v;
                        if (uv_is_v2) {
                            const Vector2& uv = uv_attr->vector2(slot);
                            c.u = uv.x;
                            c.v = uv.y;
                        } else {
                            const Vector4& uv = uv_attr->vector4(slot);
                            const float invq = (uv.w != 0.0f) ? (1.0f / uv.w) : 1.0f;
                            c.u = uv.x * invq;
                            c.v = uv.y * invq;
                        }
                    } else {
                        c.u = 0.0f;
                        c.v = 0.0f;
                    }
                    return c;
                };

                const Corner c0 = resolve(face_local[0]);
                for (unsigned k = 1; k + 1 < fv; ++k) {
                    corners.push_back(c0);
                    corners.push_back(resolve(face_local[k]));
                    corners.push_back(resolve(face_local[k + 1]));
                }
            }
        }

        if (corners.size() < 3) return result;

        // 2) Fat vertices. Two key modes:
        //    - default: (pointIdx, u, v) so UV seams become topology breaks
        //    - ignore_uv_seams: pointIdx only; first-seen UV wins
        std::vector<FatVertex> fat_verts;
        std::vector<uint32_t>  indices;
        indices.reserve(corners.size());

        if (k_ignore_uv_seams) {
            std::unordered_map<uint32_t, uint32_t> dedup;
            dedup.reserve(corners.size());
            fat_verts.reserve(corners.size() / 2);
            for (const Corner& c : corners) {
                auto it = dedup.find(c.pointIdx);
                if (it != dedup.end()) {
                    indices.push_back(it->second);
                } else {
                    const uint32_t new_idx = static_cast<uint32_t>(fat_verts.size());
                    const Vector3& P = (*points_in)[c.pointIdx];
                    FatVertex fv;
                    fv.x = P.x; fv.y = P.y; fv.z = P.z;
                    fv.u = c.u; fv.v = c.v;
                    fv.srcPointIdx = c.pointIdx;
                    fat_verts.push_back(fv);
                    dedup.emplace(c.pointIdx, new_idx);
                    indices.push_back(new_idx);
                }
            }
        } else {
            std::unordered_map<FatKey, uint32_t, FatKeyHash> dedup;
            dedup.reserve(corners.size());
            fat_verts.reserve(corners.size() / 2);
            for (const Corner& c : corners) {
                FatKey key{ c.pointIdx, c.u, c.v };
                auto it = dedup.find(key);
                if (it != dedup.end()) {
                    indices.push_back(it->second);
                } else {
                    const uint32_t new_idx = static_cast<uint32_t>(fat_verts.size());
                    const Vector3& P = (*points_in)[c.pointIdx];
                    FatVertex fv;
                    fv.x = P.x; fv.y = P.y; fv.z = P.z;
                    fv.u = c.u; fv.v = c.v;
                    fv.srcPointIdx = c.pointIdx;
                    fat_verts.push_back(fv);
                    dedup.emplace(key, new_idx);
                    indices.push_back(new_idx);
                }
            }
        }

        const size_t num_verts   = fat_verts.size();
        const size_t num_indices = indices.size();

        // 2b) If pin_silhouette is on, build a vertex_lock array.
        // Algorithm: face normals -> edge map -> for each interior edge,
        // if dihedral angle > threshold, mark both endpoints as locked.
        // Open boundary edges (1 triangle) are handled by the
        // meshopt_SimplifyLockBorder flag below, so we don't double-lock
        // them here.
        std::vector<unsigned char> vertex_lock;
        if (k_pin_silhouette && !k_aggressive && num_indices >= 3) {
            const size_t num_input_tris = num_indices / 3;

            std::vector<Vector3> input_normals(num_input_tris);
            for (size_t t = 0; t < num_input_tris; ++t) {
                const FatVertex& fa = fat_verts[indices[t * 3 + 0]];
                const FatVertex& fb = fat_verts[indices[t * 3 + 1]];
                const FatVertex& fc = fat_verts[indices[t * 3 + 2]];
                const Vector3 va(fa.x, fa.y, fa.z);
                const Vector3 vb(fb.x, fb.y, fb.z);
                const Vector3 vc(fc.x, fc.y, fc.z);
                Vector3 n = (vb - va).cross(vc - va);
                const float len = n.length();
                if (len > 0.0f) n /= len;
                input_normals[t] = n;
            }

            struct EdgeRef { uint32_t tri; uint8_t edge; };
            auto edge_key = [](uint32_t a, uint32_t b) -> uint64_t {
                if (a > b) std::swap(a, b);
                return (static_cast<uint64_t>(a) << 32) | static_cast<uint64_t>(b);
            };
            std::unordered_map<uint64_t, std::vector<EdgeRef>> edge_map;
            edge_map.reserve(num_input_tris * 3);
            for (uint32_t t = 0; t < num_input_tris; ++t) {
                for (uint8_t e = 0; e < 3; ++e) {
                    const uint32_t va = indices[t * 3 + e];
                    const uint32_t vb = indices[t * 3 + ((e + 1) % 3)];
                    edge_map[edge_key(va, vb)].push_back({ t, e });
                }
            }

            vertex_lock.assign(num_verts, 0);
            const float cos_threshold =
                std::cos(k_silhouette_angle * (3.14159265358979323846f / 180.0f));

            for (auto& kv : edge_map) {
                if (kv.second.size() != 2) continue;
                const float dot = input_normals[kv.second[0].tri].dot(
                                  input_normals[kv.second[1].tri]);
                // dot < cos(threshold) means the angle BETWEEN normals is
                // larger than the threshold -- i.e. a sharp crease.
                if (dot < cos_threshold) {
                    const uint32_t a = static_cast<uint32_t>(kv.first >> 32);
                    const uint32_t b = static_cast<uint32_t>(kv.first & 0xFFFFFFFFull);
                    vertex_lock[a] = 1;
                    vertex_lock[b] = 1;
                }
            }
        }

        // 3) Simplify.
        size_t target_idx = static_cast<size_t>(
            static_cast<float>(num_indices) * k_target_ratio);
        target_idx -= (target_idx % 3);
        if (target_idx < 3) target_idx = 3;

        // Skip the simplifier entirely if ratio is 1.0 and we're only here
        // for quad merging.
        bool ran_simplifier = false;
        std::vector<uint32_t> out_indices;

        if (k_target_ratio < 0.9999f) {
            ran_simplifier = true;
            out_indices.assign(num_indices, 0);

            const float* pos_base = &fat_verts[0].x;
            const float* uv_base  = &fat_verts[0].u;
            const size_t v_stride = sizeof(FatVertex);
            float attr_weights[2] = { k_uv_weight, k_uv_weight };

            unsigned int options = 0;
            if (k_lock_borders)     options |= meshopt_SimplifyLockBorder;
            if (k_prune_components) options |= meshopt_SimplifyPrune;

            float result_error = 0.0f;
            size_t result_idx_count = 0;

            if (k_aggressive) {
                result_idx_count = meshopt_simplifySloppy(
                    out_indices.data(),
                    indices.data(), num_indices,
                    pos_base, num_verts, v_stride,
                    target_idx, k_target_error, &result_error);
            } else {
                result_idx_count = meshopt_simplifyWithAttributes(
                    out_indices.data(),
                    indices.data(), num_indices,
                    pos_base, num_verts, v_stride,
                    uv_base, v_stride,
                    attr_weights, /*attribute_count=*/2,
                    vertex_lock.empty() ? nullptr : vertex_lock.data(),
                    target_idx, k_target_error, options, &result_error);
            }

            if (result_idx_count < 3) {
                warning("MeshSimplify: object collapsed to zero triangles. "
                        "Raise target_ratio or lower max_error.");
                return result;
            }
            out_indices.resize(result_idx_count);
            (void)result_error;
        } else {
            // No simplification, pass indices through.
            out_indices = indices;
        }

        // 4) Compact surviving fat vertices.
        std::vector<uint32_t> fat_to_new(num_verts, UINT32_MAX);
        std::vector<uint32_t> surviving_fat;
        surviving_fat.reserve(num_verts);

        for (uint32_t& idx : out_indices) {
            uint32_t& slot = fat_to_new[idx];
            if (slot == UINT32_MAX) {
                slot = static_cast<uint32_t>(surviving_fat.size());
                surviving_fat.push_back(idx);
            }
            idx = slot;
        }

        // 5) Collapse fat verts back to Nuke point list (one point per
        // unique srcPointIdx; UV seam siblings share a point).
        std::unordered_map<uint32_t, uint32_t> src_to_point;
        src_to_point.reserve(surviving_fat.size());
        std::vector<uint32_t> fat_to_point(surviving_fat.size());
        result.points.reserve(surviving_fat.size());

        for (size_t i = 0; i < surviving_fat.size(); ++i) {
            const FatVertex& fv = fat_verts[surviving_fat[i]];
            auto it = src_to_point.find(fv.srcPointIdx);
            if (it == src_to_point.end()) {
                const uint32_t np = static_cast<uint32_t>(result.points.size());
                src_to_point.emplace(fv.srcPointIdx, np);
                result.points.emplace_back(fv.x, fv.y, fv.z);
                fat_to_point[i] = np;
            } else {
                fat_to_point[i] = it->second;
            }
        }

        // 6) Emit triangle indices and per-corner UVs into result.
        const size_t num_new_tris = out_indices.size() / 3;
        result.tri_indices.resize(num_new_tris * 3);
        if (result.has_uv) result.tri_uvs.resize(num_new_tris * 3);

        for (size_t t = 0; t < num_new_tris; ++t) {
            for (int k = 0; k < 3; ++k) {
                const uint32_t new_fat = out_indices[t * 3 + k];
                result.tri_indices[t * 3 + k] = fat_to_point[new_fat];
                if (result.has_uv) {
                    const FatVertex& fv = fat_verts[surviving_fat[new_fat]];
                    result.tri_uvs[t * 3 + k] = Vector4(fv.u, fv.v, 0.0f, 1.0f);
                }
            }
        }

        // 7) Optional quad merge.
        if (k_output_quads && num_new_tris >= 2) {
            const float angle_rad = k_quad_angle * (3.14159265358979323846f / 180.0f);
            mergeCoplanarTriPairs(
                result.tri_indices, result.tri_uvs,
                result.quad_indices, result.quad_uvs,
                result.points,
                angle_rad,
                result.has_uv);
        }

        (void)ran_simplifier;
        result.valid = true;
        return result;
    }
};

// ---------------------------------------------------------------------------

static Op* build(Node* node) { return new MeshSimplify(node); }

const Op::Description MeshSimplify::description(
    kClass, "Geometry/MeshSimplify", build);

} // anonymous namespace
