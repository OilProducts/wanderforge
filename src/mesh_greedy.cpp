#include "mesh.h"
#include "chunk.h"
#include "wf_math.h"
#include <vector>

namespace wf {

struct MaskCell {
    uint16_t mat;
    int8_t sign; // +1 or -1
};

static inline bool cell_equal(const MaskCell& a, const MaskCell& b) {
    return a.mat == b.mat && a.sign == b.sign;
}

static void add_quad(Mesh& m, Float3 origin, Float3 udir, Float3 vdir, Float3 n, float w, float h, uint16_t mat, bool flip) {
    const uint32_t base = (uint32_t)m.vertices.size();
    Float3 p0 = origin;
    Float3 p1 = { origin.x + udir.x * w, origin.y + udir.y * w, origin.z + udir.z * w };
    Float3 p2 = { p1.x + vdir.x * h, p1.y + vdir.y * h, p1.z + vdir.z * h };
    Float3 p3 = { origin.x + vdir.x * h, origin.y + vdir.y * h, origin.z + vdir.z * h };
    Vertex v0{p0.x, p0.y, p0.z, n.x, n.y, n.z, mat};
    Vertex v1{p1.x, p1.y, p1.z, n.x, n.y, n.z, mat};
    Vertex v2{p2.x, p2.y, p2.z, n.x, n.y, n.z, mat};
    Vertex v3{p3.x, p3.y, p3.z, n.x, n.y, n.z, mat};
    m.vertices.push_back(v0);
    m.vertices.push_back(v1);
    m.vertices.push_back(v2);
    m.vertices.push_back(v3);
    if (!flip) {
        m.indices.push_back(base + 0);
        m.indices.push_back(base + 1);
        m.indices.push_back(base + 2);
        m.indices.push_back(base + 0);
        m.indices.push_back(base + 2);
        m.indices.push_back(base + 3);
    } else {
        m.indices.push_back(base + 0);
        m.indices.push_back(base + 2);
        m.indices.push_back(base + 1);
        m.indices.push_back(base + 0);
        m.indices.push_back(base + 3);
        m.indices.push_back(base + 2);
    }
}

static inline bool get_neighbor_solid(const Chunk64* n, int x, int y, int z) {
    if (!n) return false; // treat as air when no neighbor provided (no outer walls)
    if (x < 0 || y < 0 || z < 0 || x >= Chunk64::N || y >= Chunk64::N || z >= Chunk64::N) return false;
    return n->is_solid(x, y, z);
}

void mesh_chunk_greedy_neighbors(const Chunk64& c,
                                 const Chunk64* negX, const Chunk64* posX,
                                 const Chunk64* negY, const Chunk64* posY,
                                 const Chunk64* negZ, const Chunk64* posZ,
                                 Mesh& out, float s) {
    out.vertices.clear(); out.indices.clear();
    const int N = Chunk64::N;
    // For each axis
    for (int axis = 0; axis < 3; ++axis) {
        int du = (axis == 0) ? 2 : 0; // u index maps: for X axis, u=z; Y axis, u=x; Z axis, u=x
        int dv = (axis == 0) ? 1 : ((axis == 1) ? 2 : 1); // for X axis, v=y; Y axis, v=z; Z axis, v=y
        int dims[3] = { N, N, N };
        int U = dims[du], V = dims[dv];
        std::vector<MaskCell> mask(U * V);
        std::vector<uint8_t> taken(U * V);
        // Sweep layers between cells (N+1 planes)
        for (int d = 0; d <= N; ++d) {
            // Build mask at this plane
            for (int v = 0; v < V; ++v) {
                for (int u = 0; u < U; ++u) {
                    int acoord = d - 1;
                    int bcoord = d;
                    // Compose coords for a and b
                    int ax = 0, ay = 0, az = 0, bx = 0, by = 0, bz = 0;
                    if (axis == 0) { // X
                        ax = acoord; bx = bcoord; ay = v; by = v; az = u; bz = u;
                    } else if (axis == 1) { // Y
                        ay = acoord; by = bcoord; ax = u; bx = u; az = v; bz = v;
                    } else { // Z
                        az = acoord; bz = bcoord; ax = u; bx = u; ay = v; by = v;
                    }
                    bool a_in = (acoord >= 0 && acoord < N);
                    bool b_in = (bcoord >= 0 && bcoord < N);
                    MaskCell cell{0, 0};
                    if (a_in && b_in) {
                        bool a_sol = c.is_solid(ax, ay, az);
                        bool b_sol = c.is_solid(bx, by, bz);
                        if (a_sol != b_sol) {
                            if (a_sol) { cell.mat = c.get_material(ax, ay, az); cell.sign = +1; }
                            else       { cell.mat = c.get_material(bx, by, bz); cell.sign = -1; }
                        }
                    } else if (a_in && !b_in) {
                        // Positive chunk boundary (d == N): consult neighbor to decide seam face; if no neighbor, do not emit (avoid outer walls)
                        bool a_sol = c.is_solid(ax, ay, az);
                        bool has_nb = (axis == 0) ? (posX != nullptr)
                                      : (axis == 1) ? (posY != nullptr)
                                                    : (posZ != nullptr);
                        if (has_nb) {
                            bool b_sol = false;
                            if (axis == 0)      b_sol = get_neighbor_solid(posX, 0, by, bz);
                            else if (axis == 1) b_sol = get_neighbor_solid(posY, bx, 0, bz);
                            else                b_sol = get_neighbor_solid(posZ, bx, by, 0);
                            if (a_sol != b_sol && a_sol) { cell.mat = c.get_material(ax, ay, az); cell.sign = +1; }
                        } else {
                            cell = {0, 0};
                        }
                    } else if (!a_in && b_in) {
                        // Negative chunk boundary (d == 0): consult neighbor to decide seam face; if no neighbor, do not emit (avoid outer walls)
                        bool has_nb = (axis == 0) ? (negX != nullptr)
                                      : (axis == 1) ? (negY != nullptr)
                                                    : (negZ != nullptr);
                        if (has_nb) {
                            bool b_sol = c.is_solid(bx, by, bz);
                            bool a_sol = false;
                            if (axis == 0)      a_sol = get_neighbor_solid(negX, Chunk64::N - 1, by, bz);
                            else if (axis == 1) a_sol = get_neighbor_solid(negY, bx, Chunk64::N - 1, bz);
                            else                a_sol = get_neighbor_solid(negZ, bx, by, Chunk64::N - 1);
                            if (b_sol != a_sol && b_sol) { cell.mat = c.get_material(bx, by, bz); cell.sign = -1; }
                        } else {
                            cell = {0, 0};
                        }
                    }
                    mask[u + v * U] = cell;
                }
            }
            std::fill(taken.begin(), taken.end(), 0);
            // Greedy merge
            for (int v = 0; v < V; ++v) {
                for (int u = 0; u < U; ) {
                    const MaskCell c0 = mask[u + v * U];
                    if (taken[u + v * U] || c0.mat == 0) { ++u; continue; }
                    // width
                    int w = 1;
                    while (u + w < U && !taken[u + w + v * U] && cell_equal(mask[u + w + v * U], c0)) ++w;
                    // height
                    int h = 1;
                    bool done = false;
                    while (v + h < V && !done) {
                        for (int x = 0; x < w; ++x) {
                            if (taken[u + x + (v + h) * U] || !cell_equal(mask[u + x + (v + h) * U], c0)) { done = true; break; }
                        }
                        if (!done) ++h;
                    }
                    // mark
                    for (int y = 0; y < h; ++y) {
                        for (int x = 0; x < w; ++x) taken[u + x + (v + y) * U] = 1;
                    }
                    // emit quad
                    Float3 n{0,0,0};
                    if (axis == 0) n = (c0.sign > 0) ? Float3{1,0,0} : Float3{-1,0,0};
                    if (axis == 1) n = (c0.sign > 0) ? Float3{0,1,0} : Float3{0,-1,0};
                    if (axis == 2) n = (c0.sign > 0) ? Float3{0,0,1} : Float3{0,0,-1};
                    Float3 origin{0,0,0}, udir{0,0,0}, vdir{0,0,0};
                    float plane = d * s;
                    if (axis == 0) {
                        origin = Float3{plane, v * s, u * s};
                        udir = Float3{0, 0, 1}; // Z (du)
                        vdir = Float3{0, 1, 0}; // Y (dv)
                    } else if (axis == 1) {
                        origin = Float3{u * s, plane, v * s};
                        udir = Float3{1, 0, 0}; // X (du)
                        vdir = Float3{0, 0, 1}; // Z (dv)
                    } else {
                        origin = Float3{u * s, v * s, plane};
                        udir = Float3{1, 0, 0}; // X (du)
                        vdir = Float3{0, 1, 0}; // Y (dv)
                    }
                    // Flip indices depending on axis/sign to make outward faces front-facing under CLOCKWISE
                    bool flip = (axis == 0 || axis == 1) ? (c0.sign > 0) : (c0.sign < 0);
                    add_quad(out, origin, udir, vdir, n, w * s, h * s, c0.mat, flip);
                    u += w;
                }
            }
        }
    }
}

// Backward-compatible entry: no neighbors considered
void mesh_chunk_greedy(const Chunk64& c, Mesh& out, float s) {
    mesh_chunk_greedy_neighbors(c, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, out, s);
}

} // namespace wf
