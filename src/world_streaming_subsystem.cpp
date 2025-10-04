#include "world_streaming_subsystem.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "mesh.h"
#include "planet.h"
#include "wf_math.h"
#include "wf_noise.h"

namespace wf {

void WorldStreamingSubsystem::configure(const PlanetConfig& planet_cfg,
                                        const std::string& region_root,
                                        bool save_chunks,
                                        bool log_stream,
                                        std::size_t remesh_per_frame_cap,
                                        std::size_t worker_count_hint) {
    manager_.set_planet_config(planet_cfg);
    manager_.set_region_root(region_root);
    manager_.set_save_chunks_enabled(save_chunks);
    manager_.set_log_stream(log_stream);
    manager_.set_remesh_per_frame_cap(remesh_per_frame_cap);
    manager_.set_worker_count(worker_count_hint);

    worker_count_hint_ = worker_count_hint;

    manager_.set_load_job([this](const LoadRequest& req) {
        this->build_ring_job(req);
    });
}

void WorldStreamingSubsystem::apply_runtime_settings(float surface_push_m,
                                                     bool debug_chunk_keys,
                                                     bool profile_enabled,
                                                     std::function<void(const std::string&)> profile_sink,
                                                     std::chrono::steady_clock::time_point profile_start_tp) {
    surface_push_m_ = surface_push_m;
    debug_chunk_keys_ = debug_chunk_keys;
    profile_enabled_ = profile_enabled;
    profile_sink_ = std::move(profile_sink);
    profile_start_tp_ = profile_start_tp;
}

void WorldStreamingSubsystem::set_load_job(ChunkStreamingManager::LoadJob job) {
    manager_.set_load_job(std::move(job));
}

void WorldStreamingSubsystem::start() {
    manager_.start();
}

void WorldStreamingSubsystem::stop() {
    manager_.stop();
}

void WorldStreamingSubsystem::wait_for_pending_saves() {
    manager_.wait_for_pending_saves();
}

uint64_t WorldStreamingSubsystem::enqueue_request(LoadRequest req) {
    return manager_.enqueue_request(std::move(req));
}

bool WorldStreamingSubsystem::should_abort(uint64_t job_gen) const {
    return manager_.should_abort(job_gen);
}

void WorldStreamingSubsystem::push_mesh_result(MeshResult res) {
    manager_.push_mesh_result(std::move(res));
}

bool WorldStreamingSubsystem::try_pop_result(MeshResult& out) {
    return manager_.try_pop_result(out);
}

void WorldStreamingSubsystem::update_generation_stats(double gen_ms, int chunks) {
    manager_.update_generation_stats(gen_ms, chunks);
}

void WorldStreamingSubsystem::update_mesh_stats(double mesh_ms, int meshed, double total_ms) {
    manager_.update_mesh_stats(mesh_ms, meshed, total_ms);
}

void WorldStreamingSubsystem::erase_chunk(const FaceChunkKey& key) {
    manager_.erase_chunk(key);
}

void WorldStreamingSubsystem::overlay_chunk_delta(const FaceChunkKey& key, Chunk64& chunk) {
    manager_.overlay_chunk_delta(key, chunk);
}

void WorldStreamingSubsystem::flush_dirty_chunk_deltas() {
    manager_.flush_dirty_chunk_deltas();
}

void WorldStreamingSubsystem::queue_remesh(const FaceChunkKey& key) {
    std::scoped_lock lock(manager_.remesh_mutex());
    manager_.remesh_queue().push_back(key);
}

std::deque<FaceChunkKey> WorldStreamingSubsystem::take_remesh_batch(std::size_t max_count) {
    std::deque<FaceChunkKey> batch;
    std::scoped_lock lock(manager_.remesh_mutex());
    auto& queue = manager_.remesh_queue();
    std::size_t taken = 0;
    while (!queue.empty() && taken < max_count) {
        batch.push_back(queue.front());
        queue.pop_front();
        ++taken;
    }
    return batch;
}

std::optional<Chunk64> WorldStreamingSubsystem::find_chunk_copy(const FaceChunkKey& key) const {
    std::optional<Chunk64> copy;
    manager_.with_chunk(key, [&](const Chunk64& chunk) {
        copy = chunk;
    });
    return copy;
}

void WorldStreamingSubsystem::store_chunk(const FaceChunkKey& key, const Chunk64& chunk) {
    manager_.store_chunk(key, chunk);
}

ChunkDelta WorldStreamingSubsystem::load_delta_copy(const FaceChunkKey& key) const {
    std::scoped_lock lock(manager_.chunk_delta_mutex());
    auto it = manager_.chunk_deltas().find(key);
    if (it != manager_.chunk_deltas().end()) {
        return it->second;
    }
    return ChunkDelta{};
}

WorldStreamingSubsystem::NeighborChunks WorldStreamingSubsystem::gather_neighbor_chunks(const FaceChunkKey& key) const {
    NeighborChunks neighbors;
    manager_.visit_neighbors(key, [&](const FaceChunkKey& neighbor_key, const Chunk64* chunk) {
        if (!chunk) return;
        int di = neighbor_key.i - key.i;
        int dj = neighbor_key.j - key.j;
        int dk = neighbor_key.k - key.k;
        if (di == -1 && dj == 0 && dk == 0) neighbors.neg_x = *chunk;
        else if (di == 1 && dj == 0 && dk == 0) neighbors.pos_x = *chunk;
        else if (di == 0 && dj == -1 && dk == 0) neighbors.neg_y = *chunk;
        else if (di == 0 && dj == 1 && dk == 0) neighbors.pos_y = *chunk;
        else if (di == 0 && dj == 0 && dk == -1) neighbors.neg_z = *chunk;
        else if (di == 0 && dj == 0 && dk == 1) neighbors.pos_z = *chunk;
    });
    return neighbors;
}

bool WorldStreamingSubsystem::build_chunk_mesh(const FaceChunkKey& key,
                                               const Chunk64& chunk,
                                               const Chunk64* nx,
                                               const Chunk64* px,
                                               const Chunk64* ny,
                                               const Chunk64* py,
                                               const Chunk64* nz,
                                               const Chunk64* pz,
                                               MeshResult& out) const {
    return build_chunk_mesh_result(key, chunk, nx, px, ny, py, nz, pz, out);
}

bool WorldStreamingSubsystem::build_chunk_mesh(const FaceChunkKey& key,
                                               const Chunk64& chunk,
                                               MeshResult& out) const {
    NeighborChunks neighbors = gather_neighbor_chunks(key);
    return build_chunk_mesh_result(key,
                                   chunk,
                                   neighbors.nx_ptr(),
                                   neighbors.px_ptr(),
                                   neighbors.ny_ptr(),
                                   neighbors.py_ptr(),
                                   neighbors.nz_ptr(),
                                   neighbors.pz_ptr(),
                                   out);
}

void WorldStreamingSubsystem::build_ring_job(const LoadRequest& request) {
    const int face = request.face;
    const int ring_radius = request.ring_radius;
    const std::int64_t center_i = request.ci;
    const std::int64_t center_j = request.cj;
    const std::int64_t center_k = request.ck;
    const int k_down = request.k_down;
    const int k_up = request.k_up;
    const float fwd_s = request.fwd_s;
    const float fwd_t = request.fwd_t;
    const uint64_t job_gen = request.gen;

    const PlanetConfig& cfg = manager_.planet_config();
    const int N = Chunk64::N;
    const double chunk_m = static_cast<double>(N) * cfg.voxel_size_m;

    Float3 right, up, forward;
    face_basis(face, right, up, forward);

    const int tile_span = ring_radius;
    const int W = 2 * tile_span + 1;
    const int KD = k_down + k_up + 1;
    auto idx_of = [&](int di, int dj, int dk) {
        int ix = di + tile_span;
        int jy = dj + tile_span;
        int kz = dk + k_down;
        return (kz * W + jy) * W + ix;
    };
    std::vector<Chunk64> chunks(W * W * KD);

    struct Off {
        int di;
        int dj;
        int dist2;
        float dot;
    };
    std::vector<Off> order;
    order.reserve(W * W);

    float len = std::sqrt(fwd_s * fwd_s + fwd_t * fwd_t);
    float dir_s = (len > 1e-6f) ? (fwd_s / len) : 0.0f;
    float dir_t = (len > 1e-6f) ? (fwd_t / len) : 0.0f;
    for (int dj = -tile_span; dj <= tile_span; ++dj) {
        for (int di = -tile_span; di <= tile_span; ++di) {
            int d2 = di * di + dj * dj;
            float dot = di * dir_s + dj * dir_t;
            order.push_back(Off{di, dj, d2, dot});
        }
    }
    std::sort(order.begin(), order.end(), [&](const Off& a, const Off& b) {
        if (a.dist2 != b.dist2) return a.dist2 < b.dist2;
        if (a.dot != b.dot) return a.dot > b.dot;
        if (a.dj != b.dj) return a.dj < b.dj;
        return a.di < b.di;
    });

    struct Task {
        int di;
        int dj;
        int dk;
    };
    std::vector<Task> tasks;
    tasks.reserve(W * W * KD);
    for (int dj = -tile_span; dj <= tile_span; ++dj) {
        for (int di = -tile_span; di <= tile_span; ++di) {
            for (int dk = -k_down; dk <= k_up; ++dk) {
                tasks.push_back(Task{di, dj, dk});
            }
        }
    }

    auto t0 = std::chrono::steady_clock::now();
    std::atomic<size_t> task_index{0};
    int nthreads = worker_count_hint_ > 0 ? static_cast<int>(worker_count_hint_)
                                          : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    std::vector<std::thread> workers;
    workers.reserve(nthreads);
    for (int w = 0; w < nthreads; ++w) {
        workers.emplace_back([&, job_gen]() {
            for (;;) {
                if (manager_.should_abort(job_gen)) return;
                size_t idx = task_index.fetch_add(1, std::memory_order_relaxed);
                if (idx >= tasks.size()) break;
                const Task task = tasks[idx];
                int di = task.di;
                int dj = task.dj;
                int dk = task.dk;
                std::int64_t kk = center_k + dk;
                FaceChunkKey key{face, center_i + di, center_j + dj, kk};
                if (debug_chunk_keys_) {
                    static std::atomic<int> debug_chunk_log_count{0};
                    if (debug_chunk_log_count.load(std::memory_order_relaxed) < 32) {
                        int prev = debug_chunk_log_count.fetch_add(1, std::memory_order_relaxed);
                        if (prev < 32) {
                            std::cout << "[chunk-load] face=" << key.face
                                      << " i=" << key.i << " j=" << key.j << " k=" << key.k << '\n';
                        }
                    }
                }
                Chunk64& chunk = chunks[idx_of(di, dj, dk)];
                generate_base_chunk(key, right, up, forward, chunk);
                manager_.overlay_chunk_delta(key, chunk);
                manager_.store_chunk(key, chunk);
            }
        });
    }
    for (auto& th : workers) {
        th.join();
    }

    auto t1 = std::chrono::steady_clock::now();
    double gen_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    manager_.update_generation_stats(gen_ms, static_cast<int>(tasks.size()));
    if (manager_.should_abort(job_gen)) return;

    Float3 fwd_world_cam = normalize(Float3{fwd_s * right.x + fwd_t * up.x + forward.x,
                                            fwd_s * right.y + fwd_t * up.y + forward.y,
                                            fwd_s * right.z + fwd_t * up.z + forward.z});
    constexpr float kDegToRad = 0.01745329251994329577f;
    float cone_cos = std::cos(75.0f * kDegToRad);

    struct MeshTask {
        int di;
        int dj;
        int dk;
    };
    std::vector<MeshTask> mtasks;
    mtasks.reserve(order.size() * KD);
    for (const Off& off : order) {
        for (int dk = -k_down; dk <= k_up; ++dk) {
            mtasks.push_back(MeshTask{off.di, off.dj, dk});
        }
    }

    std::atomic<size_t> mesh_index{0};
    std::atomic<int> meshed_accum{0};
    std::vector<std::thread> mesh_workers;
    mesh_workers.reserve(nthreads);
    auto mesh_worker = [&]() {
        int local_meshed = 0;
        while (true) {
            if (manager_.should_abort(job_gen)) break;
            size_t idx = mesh_index.fetch_add(1, std::memory_order_relaxed);
            if (idx >= mtasks.size()) break;
            const MeshTask mt = mtasks[idx];
            int di = mt.di;
            int dj = mt.dj;
            int dk = mt.dk;
            std::int64_t kk = center_k + dk;
            const Chunk64& chunk = chunks[idx_of(di, dj, dk)];

            float Sc = static_cast<float>((center_i + di) * chunk_m + (chunk_m * 0.5));
            float Tc = static_cast<float>((center_j + dj) * chunk_m + (chunk_m * 0.5));
            float Rc = static_cast<float>((kk) * chunk_m + (chunk_m * 0.5));
            float cr = Sc / Rc;
            float cu = Tc / Rc;
            float cf = std::sqrt(std::max(0.0f, 1.0f - (cr * cr + cu * cu)));
            Float3 dirc = normalize(Float3{right.x * cr + up.x * cu + forward.x * cf,
                                           right.y * cr + up.y * cu + forward.y * cf,
                                           right.z * cr + up.z * cu + forward.z * cf});
            float dcam = fwd_world_cam.x * dirc.x + fwd_world_cam.y * dirc.y + fwd_world_cam.z * dirc.z;
            if (!debug_chunk_keys_ && dcam < cone_cos) {
                continue;
            }

            const Chunk64* nx = (di > -tile_span) ? &chunks[idx_of(di - 1, dj, dk)] : nullptr;
            const Chunk64* px = (di < tile_span) ? &chunks[idx_of(di + 1, dj, dk)] : nullptr;
            const Chunk64* ny = (dj > -tile_span) ? &chunks[idx_of(di, dj - 1, dk)] : nullptr;
            const Chunk64* py = (dj < tile_span) ? &chunks[idx_of(di, dj + 1, dk)] : nullptr;
            const Chunk64* nz = (dk > -k_down) ? &chunks[idx_of(di, dj, dk - 1)] : nullptr;
            const Chunk64* pz = (dk < k_up) ? &chunks[idx_of(di, dj, dk + 1)] : nullptr;

            MeshResult result;
            if (!build_chunk_mesh_result(FaceChunkKey{face, center_i + di, center_j + dj, kk},
                                         chunk, nx, px, ny, py, nz, pz, result)) {
                continue;
            }
            result.job_gen = job_gen;
            manager_.push_mesh_result(std::move(result));
            ++local_meshed;
        }
        if (local_meshed) {
            meshed_accum.fetch_add(local_meshed, std::memory_order_relaxed);
        }
    };
    for (int w = 0; w < nthreads; ++w) {
        mesh_workers.emplace_back(mesh_worker);
    }
    for (auto& th : mesh_workers) {
        th.join();
    }

    int meshed_count = meshed_accum.load(std::memory_order_relaxed);
    auto t2 = std::chrono::steady_clock::now();
    double mesh_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    manager_.update_mesh_stats(mesh_ms, meshed_count, gen_ms + mesh_ms);

    if (profile_enabled_ && profile_sink_) {
        double tsec = std::chrono::duration<double>(t2 - profile_start_tp_).count();
        char line[256];
        std::snprintf(line, sizeof(line),
                      "job,%.3f,%d,%d,%.3f,%.3f,%.3f\n",
                      tsec,
                      static_cast<int>(tasks.size()),
                      meshed_count,
                      gen_ms,
                      mesh_ms,
                      gen_ms + mesh_ms);
        profile_sink_(line);
    }
}

void WorldStreamingSubsystem::generate_base_chunk(const FaceChunkKey& key,
                                                  const Float3& right,
                                                  const Float3& up,
                                                  const Float3& forward,
                                                  Chunk64& chunk) {
    const PlanetConfig& cfg = manager_.planet_config();
    const int N = Chunk64::N;
    const double voxel_m = cfg.voxel_size_m;
    const double chunk_m = static_cast<double>(N) * voxel_m;
    const double s_origin = static_cast<double>(key.i) * chunk_m;
    const double t_origin = static_cast<double>(key.j) * chunk_m;
    const double r_origin = static_cast<double>(key.k) * chunk_m;

    chunk.fill_all_air();

    std::array<double, Chunk64::N> radial_m{};
    for (int z = 0; z < N; ++z) {
        radial_m[z] = r_origin + (z + 0.5) * voxel_m;
    }

    struct ColumnGenData {
        Float3 dir_unit;
        double surface_r;
    };
    std::vector<ColumnGenData> column_cache(N * N);

    const double r_reference = std::max(cfg.radius_m, r_origin + 0.5 * chunk_m);
    const uint32_t cave_seed = cfg.seed + 777u;

    for (int y = 0; y < N; ++y) {
        double t0 = t_origin + (y + 0.5) * voxel_m;
        for (int x = 0; x < N; ++x) {
            double s0 = s_origin + (x + 0.5) * voxel_m;
            Float3 dir_cart{
                static_cast<float>(right.x * s0 + up.x * t0 + forward.x * r_reference),
                static_cast<float>(right.y * s0 + up.y * t0 + forward.y * r_reference),
                static_cast<float>(right.z * s0 + up.z * t0 + forward.z * r_reference)};
            Float3 dir_unit = normalize(dir_cart);

            ColumnGenData data{};
            data.dir_unit = dir_unit;
            double surface_h = terrain_height_m(cfg, dir_unit);
            data.surface_r = cfg.radius_m + surface_h;
            column_cache[y * N + x] = data;
        }
    }

    constexpr double kWaterBandDepthM = 5.0;
    constexpr double kDirtDepthM = 2.0;
    constexpr double kCaveDepthM = 3.0;
    constexpr float kCaveScale = 0.05f;
    constexpr float kCaveThreshold = 0.35f;

    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const ColumnGenData& col = column_cache[y * N + x];
            const Float3 dir = col.dir_unit;
            const float dir_x = dir.x;
            const float dir_y = dir.y;
            const float dir_z = dir.z;
            for (int z = 0; z < N; ++z) {
                double r0 = radial_m[z];
                uint16_t mat = MAT_AIR;
                if (r0 <= col.surface_r) {
                    double depth = col.surface_r - r0;
                    if (r0 > cfg.sea_level_m && depth < kWaterBandDepthM) {
                        mat = MAT_WATER;
                    } else {
                        bool carve_cave = false;
                        if (depth > kCaveDepthM) {
                            float radius_f = static_cast<float>(r0);
                            float px = dir_x * radius_f;
                            float py = dir_y * radius_f;
                            float pz = dir_z * radius_f;
                            Float3 cave_pt{px * kCaveScale, py * kCaveScale, pz * kCaveScale};
                            float cave = fbm(cave_pt, 4, 2.2f, 0.5f, cave_seed);
                            carve_cave = (cave > kCaveThreshold);
                        }
                        if (carve_cave) {
                            mat = MAT_AIR;
                        } else if (depth < kDirtDepthM) {
                            mat = MAT_DIRT;
                        } else {
                            mat = MAT_ROCK;
                        }
                    }
                }
                chunk.set_voxel(x, y, z, mat);
            }
        }
    }
}

bool WorldStreamingSubsystem::build_chunk_mesh_result(const FaceChunkKey& key,
                                                      const Chunk64& chunk,
                                                      const Chunk64* nx,
                                                      const Chunk64* px,
                                                      const Chunk64* ny,
                                                      const Chunk64* py,
                                                      const Chunk64* nz,
                                                      const Chunk64* pz,
                                                      MeshResult& out) const {
    const PlanetConfig& cfg = manager_.planet_config();
    const int N = Chunk64::N;
    const float voxel_m = static_cast<float>(cfg.voxel_size_m);
    const float chunk_m = voxel_m * static_cast<float>(N);
    const float halfm = chunk_m * 0.5f;

    Float3 right, up, forward;
    face_basis(key.face, right, up, forward);

    float S0 = static_cast<float>(key.i * chunk_m);
    float T0 = static_cast<float>(key.j * chunk_m);
    float R0 = static_cast<float>(key.k * chunk_m);
    float Sc = S0 + halfm;
    float Tc = T0 + halfm;
    float Rc = R0 + halfm;
    if (Rc <= 0.0f) Rc = halfm;

    float cr = Sc / Rc;
    float cu = Tc / Rc;
    float cf = std::sqrt(std::max(0.0f, 1.0f - (cr * cr + cu * cu)));
    Float3 dirc = normalize(Float3{right.x * cr + up.x * cu + forward.x * cf,
                                   right.y * cr + up.y * cu + forward.y * cf,
                                   right.z * cr + up.z * cu + forward.z * cf});

    Mesh mesh;
    mesh_chunk_greedy_neighbors(chunk, nx, px, ny, py, nz, pz, mesh, voxel_m);
    if (mesh.indices.empty()) return false;

    for (auto& vert : mesh.vertices) {
        Float3 lp{vert.x, vert.y, vert.z};
        float S = S0 + lp.x;
        float T = T0 + lp.y;
        float R = R0 + lp.z;
        float uc = (R != 0.0f) ? (S / R) : 0.0f;
        float vc = (R != 0.0f) ? (T / R) : 0.0f;
        float w2 = std::max(0.0f, 1.0f - (uc * uc + vc * vc));
        float wc = std::sqrt(w2);
        Float3 dir_sph = normalize(Float3{right.x * uc + up.x * vc + forward.x * wc,
                                          right.y * uc + up.y * vc + forward.y * wc,
                                          right.z * uc + up.z * vc + forward.z * wc});
        Float3 wp = dir_sph * R;
        vert.x = wp.x;
        vert.y = wp.y;
        vert.z = wp.z;
    }

    auto cross3 = [](Float3 a, Float3 b) -> Float3 {
        return Float3{a.y * b.z - a.z * b.y,
                      a.z * b.x - a.x * b.z,
                      a.x * b.y - a.y * b.x};
    };
    auto sub3 = [](Float3 a, Float3 b) -> Float3 {
        return Float3{a.x - b.x, a.y - b.y, a.z - b.z};
    };

    if (surface_push_m_ > 0.0f) {
        const float push = surface_push_m_;
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            uint32_t i0 = mesh.indices[i];
            uint32_t i1 = mesh.indices[i + 1];
            uint32_t i2 = mesh.indices[i + 2];
            Float3 p0{mesh.vertices[i0].x, mesh.vertices[i0].y, mesh.vertices[i0].z};
            Float3 p1{mesh.vertices[i1].x, mesh.vertices[i1].y, mesh.vertices[i1].z};
            Float3 p2{mesh.vertices[i2].x, mesh.vertices[i2].y, mesh.vertices[i2].z};
            Float3 e1 = sub3(p1, p0);
            Float3 e2 = sub3(p2, p0);
            Float3 n = normalize(cross3(e1, e2));
            Float3 r0 = normalize(p0);
            float radial = std::fabs(n.x * r0.x + n.y * r0.y + n.z * r0.z);
            if (radial > 0.8f) {
                Float3 push_vec{r0.x * push, r0.y * push, r0.z * push};
                mesh.vertices[i0].x = p0.x + push_vec.x;
                mesh.vertices[i0].y = p0.y + push_vec.y;
                mesh.vertices[i0].z = p0.z + push_vec.z;
                mesh.vertices[i1].x = p1.x + push_vec.x;
                mesh.vertices[i1].y = p1.y + push_vec.y;
                mesh.vertices[i1].z = p1.z + push_vec.z;
                mesh.vertices[i2].x = p2.x + push_vec.x;
                mesh.vertices[i2].y = p2.y + push_vec.y;
                mesh.vertices[i2].z = p2.z + push_vec.z;
            }
        }
    }

    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        uint32_t i0 = mesh.indices[i];
        uint32_t i1 = mesh.indices[i + 1];
        uint32_t i2 = mesh.indices[i + 2];
        const Vertex& v0 = mesh.vertices[i0];
        const Vertex& v1 = mesh.vertices[i1];
        const Vertex& v2 = mesh.vertices[i2];
        Float3 p0{v0.x, v0.y, v0.z};
        Float3 p1{v1.x, v1.y, v1.z};
        Float3 p2{v2.x, v2.y, v2.z};
        Float3 e1 = sub3(p1, p0);
        Float3 e2 = sub3(p2, p0);
        Float3 n = normalize(cross3(e1, e2));
        mesh.vertices[i0].nx = n.x;
        mesh.vertices[i0].ny = n.y;
        mesh.vertices[i0].nz = n.z;
        mesh.vertices[i1].nx = n.x;
        mesh.vertices[i1].ny = n.y;
        mesh.vertices[i1].nz = n.z;
        mesh.vertices[i2].nx = n.x;
        mesh.vertices[i2].ny = n.y;
        mesh.vertices[i2].nz = n.z;
    }

    out.key = key;
    out.vertices = std::move(mesh.vertices);
    out.indices = std::move(mesh.indices);
    out.center[0] = dirc.x * Rc;
    out.center[1] = dirc.y * Rc;
    out.center[2] = dirc.z * Rc;
    out.radius = halfm * 1.73205080757f;
    return true;
}

} // namespace wf
