#include "vk_app.h"

#include <GLFW/glfw3.h>
#ifdef WF_HAVE_VMA
#include <vk_mem_alloc.h>
#endif
#include <cassert>
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <iostream>
#include <cstdio>
#include <optional>
#include <vector>
#include <fstream>
#include <sstream>
#include <cmath>
#include <chrono>
#include <stdexcept>
#include <string_view>
#include <span>

#include "chunk.h"
#include "mesh.h"
#include "planet.h"
#include "wf_noise.h"
#include "region_io.h"
#include "vk_utils.h"
#include "platform_layer.h"
#include "camera.h"
#include "ui/ui_text.h"
#include "ui/ui_primitives.h"
#include "ui/ui_id.h"

namespace wf {

namespace {
constexpr float kDeltaPromoteDensity = 0.18f; // promote sparse deltas once ~18% of voxels diverge
constexpr float kDeltaDemoteDensity = 0.08f; // demote dense deltas when activity subsides

struct ResolutionOption {
    int width = 0;
    int height = 0;
    std::string_view label;
};

static constexpr std::array<ResolutionOption, 4> kResolutionOptions = {{{1280, 720, "1280 x 720 (720p)"},
                                                                        {1920, 1080, "1920 x 1080 (1080p)"},
                                                                        {2560, 1440, "2560 x 1440 (1440p)"},
                                                                        {3840, 2160, "3840 x 2160 (2160p)"}}};
static_assert(!kResolutionOptions.empty());
}

static void throw_if_failed(VkResult r, const char* msg) {
    if (r != VK_SUCCESS) {
        std::cerr << msg << " (VkResult=" << r << ")\n";
        std::abort();
    }
}

VulkanApp::VulkanApp() {
    enable_validation_ = true; // toggled by build type in future
    render_system_.bind({&renderer_, &overlay_, &chunk_renderer_});
    streaming_.configure(planet_cfg_,
                         region_root_,
                         save_chunks_enabled_,
                         log_stream_,
                         /*remesh_per_frame_cap=*/4,
                         loader_threads_ > 0 ? static_cast<std::size_t>(loader_threads_) : 0);
}

void VulkanApp::set_config_path(std::string path) {
    config_path_override_ = std::move(path);
    if (world_runtime_initialized_) {
        world_runtime_->set_cli_config_path(config_path_override_);
    }
}

void VulkanApp::set_platform(PlatformLayer* platform) {
    platform_ = platform;
}

void VulkanApp::set_world_runtime(WorldRuntime* runtime) {
    world_runtime_ = runtime;
    if (!world_runtime_) {
        world_runtime_initialized_ = false;
    }
}

void VulkanApp::shutdown_runtime() {
    if (world_runtime_initialized_ && world_runtime_) {
        world_runtime_->shutdown();
        world_runtime_initialized_ = false;
    }
}

void VulkanApp::request_reload_config() {
    reload_config_from_disk();
}

void VulkanApp::request_save_config() {
    save_active_config();
}

void VulkanApp::refresh_runtime_state() {
    if (!world_runtime_initialized_) {
        return;
    }

    runtime_config_ = world_runtime_->snapshot_config();
    camera_snapshot_ = world_runtime_->snapshot_camera();

    cam_pos_[0] = camera_snapshot_.position.x;
    cam_pos_[1] = camera_snapshot_.position.y;
    cam_pos_[2] = camera_snapshot_.position.z;

    Float3 forward = camera_snapshot_.forward;
    cam_yaw_ = std::atan2(forward.z, forward.x);
    cam_pitch_ = std::asin(std::clamp(forward.y, -1.0f, 1.0f));

    walk_mode_ = runtime_config_.walk_mode;
    invert_mouse_x_ = runtime_config_.invert_mouse_x;
    invert_mouse_y_ = runtime_config_.invert_mouse_y;
    cam_sensitivity_ = runtime_config_.cam_sensitivity;
    cam_speed_ = runtime_config_.cam_speed;
    walk_speed_ = runtime_config_.walk_speed;
    walk_pitch_max_deg_ = runtime_config_.walk_pitch_max_deg;
    walk_surface_bias_m_ = runtime_config_.walk_surface_bias_m;
    eye_height_m_ = runtime_config_.eye_height_m;
    surface_push_m_ = runtime_config_.surface_push_m;
}

void VulkanApp::initialize() {
    if (!world_runtime_) {
        throw std::runtime_error("WorldRuntime not bound to VulkanApp before initialize");
    }
    init_window();
    load_config();
    init_vulkan();
    app_start_tp_ = std::chrono::steady_clock::now();
    update_streaming_runtime_settings();
    refresh_runtime_state();
    last_time_ = glfwGetTime();
}

bool VulkanApp::should_close() const {
    return platform_ ? platform_->should_close() : true;
}

void VulkanApp::poll_events() {
    if (platform_) {
        platform_->poll_events();
    }
}

float VulkanApp::advance_time() {
    double now = glfwGetTime();
    float dt = static_cast<float>(std::max(0.0, now - last_time_));
    last_time_ = now;
    return dt;
}

VulkanApp::~VulkanApp() {
    shutdown_runtime();
    world_runtime_ = nullptr;

    flush_dirty_chunk_deltas();
    streaming_.wait_for_pending_saves();
    streaming_.stop();
    render_system_.wait_idle();

    trash_.clear();
    render_chunks_.clear();

    VkDevice device = renderer_.device();
    if (device) {
        overlay_.cleanup(device);
        overlay_initialized_ = false;
        chunk_renderer_.cleanup(device);
        chunk_renderer_initialized_ = false;
    }

    destroy_debug_axes_pipeline();
    destroy_debug_axes_buffer();
    pipeline_compute_.reset();
    pipeline_layout_compute_.reset();
    pipeline_triangle_.reset();
    pipeline_layout_.reset();

#ifdef WF_HAVE_VMA
    if (vma_allocator_) {
        vmaDestroyAllocator(vma_allocator_);
        vma_allocator_ = nullptr;
    }
#endif

    renderer_.shutdown();
}

void VulkanApp::init_window() {
    if (!platform_) {
        throw std::runtime_error("PlatformLayer not bound to VulkanApp before initialization");
    }
    platform_->get_window_size(window_width_, window_height_);
    platform_->get_framebuffer_size(framebuffer_width_, framebuffer_height_);
    if (window_width_ <= 0 || window_height_ <= 0) {
        window_width_ = 1280;
        window_height_ = 720;
        platform_->set_window_size(window_width_, window_height_);
        platform_->get_framebuffer_size(framebuffer_width_, framebuffer_height_);
    }
    hud_resolution_index_ = find_resolution_index(window_width_, window_height_);
}

void VulkanApp::set_mouse_capture(bool capture) {
    GLFWwindow* window = platform_ ? platform_->window_handle() : nullptr;
    if (!window) return;
    if (mouse_captured_ == capture) return;
    if (capture) {
        platform_->set_mouse_capture(true);
#if GLFW_VERSION_MAJOR >= 3
        if (glfwRawMouseMotionSupported()) {
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        }
#endif
    } else {
        platform_->set_mouse_capture(false);
#if GLFW_VERSION_MAJOR >= 3
        if (glfwRawMouseMotionSupported()) {
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
        }
#endif
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
    mouse_captured_ = capture;
}

void VulkanApp::init_vulkan() {
    Renderer::CreateInfo renderer_info{};
    renderer_info.window = platform_ ? platform_->window_handle() : nullptr;
    renderer_info.enable_validation = enable_validation_;
    render_system_.initialize_renderer(renderer_info);

    trash_.resize(render_system_.frame_count());

    create_compute_pipeline();

#ifdef WF_HAVE_VMA
    {
        VmaAllocatorCreateInfo aci{};
        aci.instance = render_system_.renderer().instance();
        aci.physicalDevice = render_system_.renderer().physical_device();
        aci.device = render_system_.renderer().device();
        aci.vulkanApiVersion = VK_API_VERSION_1_0;
        if (vmaCreateAllocator(&aci, &vma_allocator_) != VK_SUCCESS) {
            std::cerr << "Warning: VMA allocator creation failed; continuing without VMA.\n";
            vma_allocator_ = nullptr;
        }
    }
#endif

    rebuild_swapchain_dependents();

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(render_system_.renderer().physical_device(), &props);
    std::cout << "GPU: " << props.deviceName << " API "
              << VK_API_VERSION_MAJOR(props.apiVersion) << '.'
              << VK_API_VERSION_MINOR(props.apiVersion) << '.'
              << VK_API_VERSION_PATCH(props.apiVersion) << "\n";
    std::cout << "Queues: graphics=" << render_system_.renderer().graphics_queue_family()
              << ", present=" << render_system_.renderer().present_queue_family() << "\n";

    // Place camera slightly outside the loaded shell, looking inward (matches previous behavior)
    {
        const PlanetConfig& cfg = planet_cfg_;
        const int N = Chunk64::N;
        const double voxel_m = cfg.voxel_size_m;
        const double chunk_m = (double)N * voxel_m;
        const double half_m = chunk_m * 0.5;
        const std::int64_t k0 = (std::int64_t)std::floor(cfg.radius_m / chunk_m);
        const double R0 = (double)k0 * chunk_m;
        const double Rc = R0 + half_m;
        Float3 right, up, forward; face_basis(0, right, up, forward);
        double Sc = half_m;
        double Tc = half_m;
        float cr = (float)(Sc / Rc);
        float cu = (float)(Tc / Rc);
        float cf = std::sqrt(std::max(0.0f, 1.0f - (cr*cr + cu*cu)));
        Float3 dirc = wf::normalize(Float3{
            right.x * cr + up.x * cu + forward.x * cf,
            right.y * cr + up.y * cu + forward.y * cf,
            right.z * cr + up.z * cu + forward.z * cf
        });
        Float3 chunk_center = dirc * (float)Rc;
        float view_back = 12.0f;
        Float3 eye = dirc * (float)(Rc + view_back) + up * 2.0f;
        cam_pos_[0] = eye.x; cam_pos_[1] = eye.y; cam_pos_[2] = eye.z;
        Float3 look = wf::normalize(chunk_center - eye);
        cam_yaw_ = std::atan2(look.z, look.x);
        cam_pitch_ = std::asin(std::clamp(look.y, -1.0f, 1.0f));
        bool chunk_outside = (Rc > (eye.x*dirc.x + eye.y*dirc.y + eye.z*dirc.z));
        if (chunk_outside) {
            cam_yaw_ += 3.14159265f;
            cam_pitch_ = -cam_pitch_;
        }
        std::cout << "[spawn] look=" << look.x << "," << look.y << "," << look.z
                  << " yaw=" << cam_yaw_ << " pitch=" << cam_pitch_ << "\n";
    }
}
void VulkanApp::record_command_buffer(const Renderer::FrameContext& ctx) {
    if (!ctx.acquired || ctx.command_buffer == VK_NULL_HANDLE) {
        return;
    }
    VkCommandBuffer cmd = ctx.command_buffer;
    const uint32_t imageIndex = ctx.image_index;

    const VkExtent2D swap_extent = renderer_.swapchain_extent();

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    throw_if_failed(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer failed");

    // No-op compute dispatch before rendering
    if (pipeline_compute_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_compute_.get());
        vkCmdDispatch(cmd, 1, 1, 1);
    }

    VkClearValue clear{ { {0.02f, 0.02f, 0.06f, 1.0f} } };
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = renderer_.render_pass();
    rbi.framebuffer = renderer_.framebuffer(imageIndex);
    rbi.renderArea.offset = {0,0};
    rbi.renderArea.extent = swap_extent;
    VkClearValue clears[2];
    clears[0] = clear;
    clears[1].depthStencil = {1.0f, 0};
    rbi.clearValueCount = 2; rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    float aspect = (swap_extent.height > 0)
        ? static_cast<float>(swap_extent.width) / static_cast<float>(swap_extent.height)
        : 1.0f;
    auto P = wf::perspective_from_deg(fov_deg_, aspect, near_m_, far_m_);
    float eye_arr[3] = { (float)cam_pos_[0], (float)cam_pos_[1], (float)cam_pos_[2] };
    Float3 eye{eye_arr[0], eye_arr[1], eye_arr[2]};

    float cyaw = std::cos(cam_yaw_), syaw = std::sin(cam_yaw_);
    float cp = std::cos(cam_pitch_), sp = std::sin(cam_pitch_);

    Float3 forward{};
    Float3 up_vec{};
    Float3 right_vec{};
    wf::Mat4 V{};

    if (!walk_mode_) {
        V = wf::view_from_yaw_pitch(cam_yaw_, cam_pitch_, eye_arr);
        forward = Float3{cp * cyaw, sp, cp * syaw};
        Float3 world_up{0.0f, 1.0f, 0.0f};
        right_vec = Float3{
            forward.y * world_up.z - forward.z * world_up.y,
            forward.z * world_up.x - forward.x * world_up.z,
            forward.x * world_up.y - forward.y * world_up.x
        };
        up_vec = Float3{
            right_vec.y * forward.z - right_vec.z * forward.y,
            right_vec.z * forward.x - right_vec.x * forward.z,
            right_vec.x * forward.y - right_vec.y * forward.x
        };
    } else {
        Float3 updir = wf::normalize(eye);
        Float3 view_dir{cp * cyaw, sp, cp * syaw};
        auto cross3 = [](const Float3& a, const Float3& b) {
            return Float3{
                a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x
            };
        };
        auto dot3 = [](const Float3& a, const Float3& b) {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        };
        Float3 forward_dir = wf::normalize(view_dir);
        Float3 right_candidate = cross3(updir, forward_dir);
        if (wf::length(right_candidate) < 1e-5f) {
            Float3 fallback = std::fabs(updir.y) < 0.9f ? Float3{0.0f, 1.0f, 0.0f} : Float3{1.0f, 0.0f, 0.0f};
            right_candidate = cross3(fallback, updir);
            if (wf::length(right_candidate) < 1e-5f) {
                fallback = Float3{0.0f, 0.0f, 1.0f};
                right_candidate = cross3(fallback, updir);
            }
        }
        right_vec = cross3(updir, forward_dir);
        right_vec = wf::normalize((wf::length(right_vec) > 1e-5f) ? right_vec : right_candidate);
        up_vec = wf::normalize(cross3(forward_dir, right_vec));
        if (dot3(up_vec, updir) < 0.0f) {
            right_vec = Float3{-right_vec.x, -right_vec.y, -right_vec.z};
            up_vec = Float3{-up_vec.x, -up_vec.y, -up_vec.z};
        }
        forward = forward_dir;
        wf::Vec3 eye_v{eye.x, eye.y, eye.z};
        wf::Vec3 center{eye_v.x + forward.x, eye_v.y + forward.y, eye_v.z + forward.z};
        wf::Vec3 up_v{up_vec.x, up_vec.y, up_vec.z};
        V = wf::look_at_rh(eye_v, center, up_v);
    }

    forward = wf::normalize(forward);
    right_vec = wf::normalize(right_vec);
    up_vec = wf::normalize(up_vec);

    auto MVP = wf::mul(P, V);

    bool debugTrianglePending = debug_show_test_triangle_ && pipeline_triangle_;
    bool chunk_ready = chunk_renderer_.is_ready() && !render_chunks_.empty();

    if (chunk_ready) {
        if (debug_chunk_keys_) {
            static bool logged_clip = false;
            if (!logged_clip) {
                std::cout << "VP matrix:\n";
                for (int r = 0; r < 4; ++r) {
                    std::cout << "  ["
                              << MVP.at(r, 0) << ", "
                              << MVP.at(r, 1) << ", "
                              << MVP.at(r, 2) << ", "
                              << MVP.at(r, 3) << "]\n";
                }
                if (!render_chunks_.empty()) {
                    const auto& rc0 = render_chunks_[0];
                    wf::Vec4 c0{rc0.center[0], rc0.center[1], rc0.center[2], 1.0f};
                    wf::Vec4 view = wf::mul(V, c0);
                    auto clip = wf::mul(P, view);
                    std::cout << "eye:" << eye.x << "," << eye.y << "," << eye.z
                              << "  forward:" << forward.x << "," << forward.y << "," << forward.z << "\n";
                    std::cout << "chunk0 center:" << rc0.center[0] << "," << rc0.center[1] << "," << rc0.center[2]
                              << " radius:" << rc0.radius << "\n";
                    std::cout << "delta:" << (rc0.center[0]-eye.x) << "," << (rc0.center[1]-eye.y) << "," << (rc0.center[2]-eye.z) << "\n";
                    std::cout << "view-space:" << view.x << "," << view.y << "," << view.z << "," << view.w << "\n";
                    std::cout << "clip test: "
                              << clip.x << ", "
                              << clip.y << ", "
                              << clip.z << ", "
                              << clip.w << "\n";
                }
                logged_clip = true;
            }
        }

        // Prepare preallocated container and compute draw stats
        chunk_items_tmp_.clear();
        chunk_items_tmp_.reserve(render_chunks_.size());
        last_draw_total_ = (int)render_chunks_.size();
        last_draw_visible_ = 0;
        last_draw_indices_ = 0;

        if (cull_enabled_) {
            const float deg_to_rad = 0.01745329252f;
            Float3 fwd_n = forward;
            Float3 up_n = up_vec;
            Float3 right_n = right_vec;

            float tan_y = std::tan(0.5f * fov_deg_ * deg_to_rad);
            float tan_x = tan_y * aspect;

            Float3 plane_right = Float3{fwd_n.x * tan_x - right_n.x,
                                        fwd_n.y * tan_x - right_n.y,
                                        fwd_n.z * tan_x - right_n.z};
            Float3 plane_left  = Float3{fwd_n.x * tan_x + right_n.x,
                                        fwd_n.y * tan_x + right_n.y,
                                        fwd_n.z * tan_x + right_n.z};
            Float3 plane_top   = Float3{fwd_n.x * tan_y - up_n.x,
                                        fwd_n.y * tan_y - up_n.y,
                                        fwd_n.z * tan_y - up_n.z};
            Float3 plane_bottom= Float3{fwd_n.x * tan_y + up_n.x,
                                        fwd_n.y * tan_y + up_n.y,
                                        fwd_n.z * tan_y + up_n.z};

            float plane_side_norm = wf::length(plane_right);
            float plane_vert_norm = wf::length(plane_top);

            for (const auto& rc : render_chunks_) {
                float dx = rc.center[0] - eye.x;
                float dy = rc.center[1] - eye.y;
                float dz = rc.center[2] - eye.z;
                Float3 delta{dx, dy, dz};
                float dist_f = dx*fwd_n.x + dy*fwd_n.y + dz*fwd_n.z;
                // near/far
                if (dist_f + rc.radius < near_m_) continue;
                if (dist_f - rc.radius > far_m_) continue;

                float dist_right = delta.x * plane_right.x + delta.y * plane_right.y + delta.z * plane_right.z;
                if (dist_right < -rc.radius * plane_side_norm) continue;
                float dist_left = delta.x * plane_left.x + delta.y * plane_left.y + delta.z * plane_left.z;
                if (dist_left < -rc.radius * plane_side_norm) continue;
                float dist_top = delta.x * plane_top.x + delta.y * plane_top.y + delta.z * plane_top.z;
                if (dist_top < -rc.radius * plane_vert_norm) continue;
                float dist_bottom = delta.x * plane_bottom.x + delta.y * plane_bottom.y + delta.z * plane_bottom.z;
                if (dist_bottom < -rc.radius * plane_vert_norm) continue;
                ChunkDrawItem item{};
                item.vbuf = rc.vbuf.get(); item.ibuf = rc.ibuf.get(); item.index_count = rc.index_count;
                item.first_index = rc.first_index; item.base_vertex = rc.base_vertex;
                item.vertex_count = rc.vertex_count;
                item.center[0] = rc.center[0]; item.center[1] = rc.center[1]; item.center[2] = rc.center[2];
                item.radius = rc.radius;
                chunk_items_tmp_.push_back(item);
                last_draw_visible_++;
                last_draw_indices_ += rc.index_count;
            }
        } else {
            for (const auto& rc : render_chunks_) {
                ChunkDrawItem item{};
                item.vbuf = rc.vbuf.get(); item.ibuf = rc.ibuf.get(); item.index_count = rc.index_count;
                item.first_index = rc.first_index; item.base_vertex = rc.base_vertex;
                item.vertex_count = rc.vertex_count;
                item.center[0] = rc.center[0]; item.center[1] = rc.center[1]; item.center[2] = rc.center[2];
                item.radius = rc.radius;
                chunk_items_tmp_.push_back(item);
                last_draw_visible_++;
                last_draw_indices_ += rc.index_count;
            }
        }
        chunk_renderer_.record(cmd, MVP.data(), chunk_items_tmp_);
    } else if (pipeline_triangle_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_triangle_.get());
        vkCmdDraw(cmd, 3, 1, 0, 0);
        debugTrianglePending = false;
    }

    if (debug_show_axes_ && debug_axes_pipeline_ && debug_axes_vertex_count_ > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, debug_axes_pipeline_.get());
        vkCmdPushConstants(cmd, debug_axes_layout_.get(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 16, MVP.data());
        VkDeviceSize offs = 0;
        VkBuffer axes_buf = debug_axes_vbo_.get();
        vkCmdBindVertexBuffers(cmd, 0, 1, &axes_buf, &offs);
        vkCmdDraw(cmd, debug_axes_vertex_count_, 1, 0, 0);
    }

    if (debugTrianglePending) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_triangle_.get());
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    overlay_.record_draw(cmd, overlay_draw_slot_);
    vkCmdEndRenderPass(cmd);

    throw_if_failed(vkEndCommandBuffer(cmd), "vkEndCommandBuffer failed");
}

void VulkanApp::update_input(const ControllerFrameInput& frame) {
    if (!platform_) {
        return;
    }

    const PlatformInputState& input = frame.platform;
    const ControllerActions& actions = frame.actions;
    float dt = frame.dt;

    set_mouse_capture(frame.mouse_captured);

    if (frame.reload_requested) {
        request_reload_config();
    }
    if (frame.save_requested) {
        request_save_config();
    }

    if (world_runtime_initialized_) {
        refresh_runtime_state();
    }

    if (world_runtime_initialized_) {
        if (config_auto_reload_enabled_) {
            config_watch_accum_ += dt;
            if (config_watch_accum_ >= 1.0f) {
                config_watch_accum_ = 0.0f;
                if (world_runtime_->reload_config_if_file_changed()) {
                    apply_config_local(world_runtime_->active_config());
                    refresh_runtime_state();
                    hud_force_refresh_ = true;
                }
            }
        } else {
            config_watch_accum_ = 0.0f;
        }
    }

    if (frame.request_close) {
        platform_->request_close();
    }

    if (actions.walk_toggle || actions.invert_x_toggle || actions.invert_y_toggle) {
        hud_force_refresh_ = true;
    }

    if (mouse_captured_) {
        if (actions.dig_pressed) {
            VoxelHit solid{};
            VoxelHit empty{};
            if (pick_voxel(solid, empty)) {
                edit_last_solid_ = solid;
                if (empty.key.face >= 0) edit_last_empty_ = empty;
                apply_voxel_edit(solid, MAT_AIR, current_brush_dim());
            }
        }

        if (actions.place_pressed) {
            VoxelHit solid{};
            VoxelHit empty{};
            if (pick_voxel(solid, empty) && empty.key.face >= 0) {
                edit_last_empty_ = empty;
                apply_voxel_edit(empty, edit_place_material_, current_brush_dim());
            }
        }
    }

    if (world_runtime_initialized_) {
        WorldUpdateInput runtime_input{};
        runtime_input.dt = dt;
        runtime_input.move.forward = actions.move_forward;
        runtime_input.move.strafe = actions.move_strafe;
        runtime_input.move.vertical = actions.move_vertical;
        runtime_input.look.yaw_delta = frame.look_yaw_delta;
        runtime_input.look.pitch_delta = frame.look_pitch_delta;
        runtime_input.walk_mode = runtime_config_.walk_mode;
        runtime_input.sprint = actions.sprint;
        runtime_input.ground_follow = runtime_config_.walk_mode;
        runtime_input.clamp_pitch = true;
        runtime_input.toggle_walk_mode = actions.walk_toggle;
        runtime_input.toggle_invert_x = actions.invert_x_toggle;
        runtime_input.toggle_invert_y = actions.invert_y_toggle;

        WorldUpdateResult runtime_result = world_runtime_->update(runtime_input);
        apply_runtime_result(runtime_result);
        refresh_runtime_state();
    }

    ui::UIBackend::InputState ui_input{};
    double cursor_x = input.mouse_x;
    double cursor_y = input.mouse_y;
    window_width_ = input.window_width;
    window_height_ = input.window_height;
    int fb_w = input.framebuffer_width;
    int fb_h = input.framebuffer_height;
    if (fb_w > 0 && fb_h > 0) {
        framebuffer_width_ = fb_w;
        framebuffer_height_ = fb_h;
    }
    double scale_x = 1.0;
    double scale_y = 1.0;
    VkExtent2D swap_extent = render_system_.swapchain_extent();
    if (window_width_ > 0) {
        double ref_width = (swap_extent.width > 0) ? static_cast<double>(swap_extent.width)
                                                  : static_cast<double>(framebuffer_width_);
        scale_x = ref_width / static_cast<double>(window_width_);
    }
    if (window_height_ > 0) {
        double ref_height = (swap_extent.height > 0) ? static_cast<double>(swap_extent.height)
                                                    : static_cast<double>(framebuffer_height_);
        scale_y = ref_height / static_cast<double>(window_height_);
    }
    ui_input.mouse_x = cursor_x * scale_x;
    ui_input.mouse_y = cursor_y * scale_y;
    ui_input.mouse_down[0] = input.mouse_left;
    ui_input.mouse_down[1] = input.mouse_right;
    ui_input.mouse_down[2] = input.mouse_middle;
    ui_input.has_mouse = input.window_focused && !mouse_captured_;
    hud_ui_backend_.begin_frame(ui_input, hud_ui_frame_index_++);
}

void VulkanApp::update_hud(float dt) {
    // Smooth FPS
    if (dt > 0.0001f && dt < 1.0f) {
        float fps = 1.0f / dt;
        if (fps_smooth_ <= 0.0f) fps_smooth_ = fps; else fps_smooth_ = fps_smooth_ * 0.9f + fps * 0.1f;
    }
    hud_accum_ += dt;

    bool do_refresh = hud_force_refresh_ || (hud_accum_ >= 0.25);
    if (!do_refresh) return;
    hud_force_refresh_ = false;
    hud_accum_ = 0.0;

    // Format both the window title and HUD overlay strings
    char title[256];
    float yaw_deg = cam_yaw_ * 57.2957795f;
    float pitch_deg = cam_pitch_ * 57.2957795f;
    std::snprintf(title, sizeof(title),
                  "Wanderforge | FPS: %.1f | Pos: (%.1f, %.1f, %.1f) | Yaw/Pitch: (%.1f, %.1f) | InvX:%d InvY:%d | Speed: %.1f",
                  fps_smooth_, cam_pos_[0], cam_pos_[1], cam_pos_[2], yaw_deg, pitch_deg,
                  invert_mouse_x_ ? 1 : 0, invert_mouse_y_ ? 1 : 0, cam_speed_);
    if (GLFWwindow* window = platform_ ? platform_->window_handle() : nullptr) {
        glfwSetWindowTitle(window, title);
    }

    char hud[768];
    if (draw_stats_enabled_) {
        float tris_m = (float)last_draw_indices_ / 3.0f / 1.0e6f;
        VkDeviceSize v_used=0,v_cap=0,i_used=0,i_cap=0;
        if (chunk_renderer_.is_ready()) chunk_renderer_.get_pool_usage(v_used, v_cap, i_used, i_cap);
        float v_used_mb = (float)v_used / (1024.0f*1024.0f);
        float v_cap_mb  = (float)(v_cap ? v_cap : (VkDeviceSize)1) / (1024.0f*1024.0f);
        float i_used_mb = (float)i_used / (1024.0f*1024.0f);
        float i_cap_mb  = (float)(i_cap ? i_cap : (VkDeviceSize)1) / (1024.0f*1024.0f);
        size_t qdepth = streaming_.result_queue_depth();
        double gen_ms = streaming_.last_generation_ms();
        int gen_chunks = streaming_.last_generated_chunks();
        double ms_per = (gen_chunks > 0) ? (gen_ms / (double)gen_chunks) : 0.0;
        double mesh_ms = streaming_.last_mesh_ms();
        int meshed = streaming_.last_meshed_chunks();
        double mesh_ms_per = (meshed > 0) ? (mesh_ms / (double)meshed) : 0.0;
        double up_ms = last_upload_ms_;
        int up_count = last_upload_count_;
        // Ground-follow diagnostics: camera vs. target surface radius
        double cam_rd_hud = std::sqrt(cam_pos_[0]*cam_pos_[0] + cam_pos_[1]*cam_pos_[1] + cam_pos_[2]*cam_pos_[2]);
        const PlanetConfig& pcfg = planet_cfg_;
        Float3 ndir = wf::normalize(Float3{(float)cam_pos_[0], (float)cam_pos_[1], (float)cam_pos_[2]});
        double h_surf = terrain_height_m(pcfg, ndir);
        double ground_r = pcfg.radius_m + h_surf; if (ground_r < pcfg.sea_level_m) ground_r = pcfg.sea_level_m;
        double target_r = ground_r + (double)eye_height_m_ + (double)walk_surface_bias_m_;
        double dr = cam_rd_hud - target_r;
        std::snprintf(hud, sizeof(hud),
                      "FPS: %.1f\nPos:(%.1f,%.1f,%.1f)  Yaw/Pitch:(%.1f,%.1f)  InvX:%d InvY:%d  Speed:%.1f\nDraw:%d/%d  Tris:%.2fM  Cull:%s  Ring:%d  Face:%d ci:%lld cj:%lld ck:%lld  k:%d/%d  Hold:%.2fs\nQueue:%zu  Gen:%.0fms (%d ch, %.2f ms/ch)  Mesh:%.0fms (%d ch, %.2f ms/ch)  Upload:%d in %.1fms (avg %.1fms)\nRad: cam=%.1f  tgt=%.1f  d=%.2f  (eye=%.2f bias=%.2f)\nPoolV: %.1f/%.1f MB  PoolI: %.1f/%.1f MB  Loader:%s",
                       fps_smooth_,
                       cam_pos_[0], cam_pos_[1], cam_pos_[2], yaw_deg, pitch_deg,
                       invert_mouse_x_?1:0, invert_mouse_y_?1:0, cam_speed_,
                      last_draw_visible_, last_draw_total_, tris_m, cull_enabled_?"on":"off", ring_radius_,
                      streaming_.stream_face(), (long long)streaming_.ring_center_i(), (long long)streaming_.ring_center_j(), (long long)streaming_.ring_center_k(), k_down_, k_up_, (double)streaming_.face_keep_timer_s(),
                      qdepth, gen_ms, gen_chunks, ms_per,
                      mesh_ms, meshed, mesh_ms_per,
                      up_count, up_ms, upload_ms_avg_,
                      (float)cam_rd_hud, (float)target_r, (float)dr, eye_height_m_, walk_surface_bias_m_,
                      v_used_mb, v_cap_mb, i_used_mb, i_cap_mb, streaming_.loader_busy()?"busy":"idle");
    } else {
        std::snprintf(hud, sizeof(hud),
                      "FPS: %.1f\nPos:(%.1f,%.1f,%.1f)  Yaw/Pitch:(%.1f,%.1f)  InvX:%d InvY:%d  Speed:%.1f",
                      fps_smooth_, cam_pos_[0], cam_pos_[1], cam_pos_[2], yaw_deg, pitch_deg,
                      invert_mouse_x_?1:0, invert_mouse_y_?1:0, cam_speed_);
    }

size_t hud_len = std::strlen(hud);
std::snprintf(hud + hud_len, sizeof(hud) - hud_len,
              "\nDebug: Axes:%s  Tri:%s",
              debug_show_axes_ ? "on" : "off",
              debug_show_test_triangle_ ? "on" : "off");

std::string manager_path = config_path_used_;
if (world_runtime_initialized_) {
    manager_path = world_runtime_->active_config().config_path;
}
const char* auto_reload_text = config_auto_reload_enabled_ ? "on" : "off";
hud_len = std::strlen(hud);
std::snprintf(hud + hud_len, sizeof(hud) - hud_len,
              "\nConfig: %s  AutoReload:%s",
              manager_path.empty() ? "(none)" : manager_path.c_str(),
              auto_reload_text);

    // Only update overlay text if it actually changed
    if (hud_text_ != hud) {
        hud_text_.assign(hud);
    }
}

void VulkanApp::load_config() {
    if (!world_runtime_) {
        throw std::runtime_error("WorldRuntime not bound to VulkanApp before load_config");
    }
    AppConfig defaults = snapshot_config();
    if (!world_runtime_initialized_) {
        WorldRuntime::CreateParams params;
        params.deps.streaming = &streaming_;
        params.initial_config = defaults;
        if (!config_path_override_.empty()) {
            params.config_path_override = config_path_override_;
        }
        world_runtime_initialized_ = world_runtime_->initialize(params);
        if (world_runtime_initialized_) {
            world_runtime_->set_profile_sink([this](const std::string& line) {
                this->profile_append_csv(line);
            });
        }
    } else {
        if (!config_path_override_.empty()) {
            world_runtime_->set_cli_config_path(config_path_override_);
        }
        world_runtime_->reload_config();
    }

    if (world_runtime_initialized_) {
        apply_config_local(world_runtime_->active_config());
        refresh_runtime_state();
    }
}

void VulkanApp::reload_config_from_disk() {
    if (!world_runtime_) {
        return;
    }
    if (!world_runtime_initialized_) {
        load_config();
        hud_force_refresh_ = true;
        return;
    }
    if (world_runtime_->reload_config()) {
        apply_config_local(world_runtime_->active_config());
        hud_force_refresh_ = true;
    }
}

void VulkanApp::save_active_config() {
    if (!world_runtime_initialized_ || !world_runtime_) return;
    world_runtime_->apply_config(snapshot_config());
    if (world_runtime_->save_active_config()) {
        apply_config_local(world_runtime_->active_config());
        hud_force_refresh_ = true;
    }
}

AppConfig VulkanApp::snapshot_config() const {
    AppConfig cfg;
    cfg.invert_mouse_x = invert_mouse_x_;
    cfg.invert_mouse_y = invert_mouse_y_;
    cfg.cam_sensitivity = cam_sensitivity_;
    cfg.cam_speed = cam_speed_;
    cfg.fov_deg = fov_deg_;
    cfg.near_m = near_m_;
    cfg.far_m = far_m_;

    cfg.walk_mode = walk_mode_;
    cfg.eye_height_m = eye_height_m_;
    cfg.walk_speed = walk_speed_;
    cfg.walk_pitch_max_deg = walk_pitch_max_deg_;
    cfg.walk_surface_bias_m = walk_surface_bias_m_;
    cfg.surface_push_m = surface_push_m_;

    cfg.use_chunk_renderer = use_chunk_renderer_;
    cfg.ring_radius = ring_radius_;
    cfg.prune_margin = prune_margin_;
    cfg.cull_enabled = cull_enabled_;
    cfg.draw_stats_enabled = draw_stats_enabled_;

    cfg.hud_scale = hud_scale_;
    cfg.hud_shadow = hud_shadow_enabled_;
    cfg.hud_shadow_offset_px = hud_shadow_offset_px_;

    cfg.log_stream = log_stream_;
    cfg.log_pool = log_pool_;
    cfg.save_chunks_enabled = save_chunks_enabled_;
    cfg.debug_chunk_keys = debug_chunk_keys_;

    cfg.profile_csv_enabled = profile_csv_enabled_;
    cfg.profile_csv_path = profile_csv_path_;

    cfg.device_local_enabled = device_local_enabled_;
    cfg.pool_vtx_mb = pool_vtx_mb_;
    cfg.pool_idx_mb = pool_idx_mb_;

    cfg.uploads_per_frame_limit = uploads_per_frame_limit_;
    cfg.loader_threads = loader_threads_;
    cfg.k_down = k_down_;
    cfg.k_up = k_up_;
    cfg.k_prune_margin = k_prune_margin_;
    cfg.face_keep_time_cfg_s = face_keep_time_cfg_s_;

    cfg.planet_cfg = planet_cfg_;
    cfg.config_path = config_path_used_;
    cfg.region_root = region_root_;

    return cfg;
}

void VulkanApp::apply_config_local(const AppConfig& cfg) {
    invert_mouse_x_ = cfg.invert_mouse_x;
    invert_mouse_y_ = cfg.invert_mouse_y;
    cam_sensitivity_ = cfg.cam_sensitivity;
    cam_speed_ = cfg.cam_speed;
    fov_deg_ = cfg.fov_deg;
    near_m_ = cfg.near_m;
    far_m_ = cfg.far_m;

    walk_mode_ = cfg.walk_mode;
    eye_height_m_ = cfg.eye_height_m;
    walk_speed_ = cfg.walk_speed;
    walk_pitch_max_deg_ = cfg.walk_pitch_max_deg;
    walk_surface_bias_m_ = cfg.walk_surface_bias_m;
    surface_push_m_ = cfg.surface_push_m;

    use_chunk_renderer_ = cfg.use_chunk_renderer;
    ring_radius_ = cfg.ring_radius;
    prune_margin_ = cfg.prune_margin;
    cull_enabled_ = cfg.cull_enabled;
    draw_stats_enabled_ = cfg.draw_stats_enabled;

    hud_scale_ = cfg.hud_scale;
    hud_shadow_enabled_ = cfg.hud_shadow;
    hud_shadow_offset_px_ = cfg.hud_shadow_offset_px;

    log_stream_ = cfg.log_stream;
    log_pool_ = cfg.log_pool;
    save_chunks_enabled_ = cfg.save_chunks_enabled;
    debug_chunk_keys_ = cfg.debug_chunk_keys;

    profile_csv_enabled_ = cfg.profile_csv_enabled;
    profile_csv_path_ = cfg.profile_csv_path;

    device_local_enabled_ = cfg.device_local_enabled;
    pool_vtx_mb_ = cfg.pool_vtx_mb;
    pool_idx_mb_ = cfg.pool_idx_mb;

    uploads_per_frame_limit_ = cfg.uploads_per_frame_limit;
    loader_threads_ = cfg.loader_threads;
    k_down_ = cfg.k_down;
    k_up_ = cfg.k_up;
    k_prune_margin_ = cfg.k_prune_margin;
    face_keep_time_cfg_s_ = cfg.face_keep_time_cfg_s;

    planet_cfg_ = cfg.planet_cfg;
    config_path_used_ = cfg.config_path;
    region_root_ = cfg.region_root;

    hud_force_refresh_ = true;
}

void VulkanApp::apply_config(const AppConfig& cfg) {
    apply_config_local(cfg);

    if (world_runtime_initialized_) {
        world_runtime_->apply_config(cfg);
        refresh_runtime_state();
    } else {
        streaming_.configure(planet_cfg_,
                             region_root_,
                             save_chunks_enabled_,
                             log_stream_,
                             /*remesh_per_frame_cap=*/4,
                             loader_threads_ > 0 ? static_cast<std::size_t>(loader_threads_) : 0);

        std::cout << "[config] region_root=" << region_root_ << " (active)\n";
        std::cout << "[config] debug_chunk_keys=" << (debug_chunk_keys_ ? "true" : "false") << " (active)\n";

        update_streaming_runtime_settings();
    }
}

void VulkanApp::update_streaming_runtime_settings() {
    if (world_runtime_initialized_) {
        return;
    }

    std::function<void(const std::string&)> profile_sink;
    if (profile_csv_enabled_) {
        profile_sink = [this](const std::string& line) {
            this->profile_append_csv(line);
        };
    }
    streaming_.apply_runtime_settings(surface_push_m_,
                                      debug_chunk_keys_,
                                      profile_csv_enabled_,
                                      std::move(profile_sink),
                                      app_start_tp_);
}

std::size_t VulkanApp::find_resolution_index(int width, int height) const {
    for (std::size_t i = 0; i < kResolutionOptions.size(); ++i) {
        if (kResolutionOptions[i].width == width && kResolutionOptions[i].height == height) {
            return i;
        }
    }
    return (hud_resolution_index_ < kResolutionOptions.size()) ? hud_resolution_index_ : 0;
}

int VulkanApp::current_brush_dim() const {
    switch (selected_tool_) {
        case ToolSelection::SmallShovel: return 2;
        case ToolSelection::LargeShovel: return 5;
        case ToolSelection::None:
        default: return 1;
    }
}

void VulkanApp::apply_resolution_option(std::size_t index) {
    GLFWwindow* window = platform_ ? platform_->window_handle() : nullptr;
    if (kResolutionOptions.empty() || index >= kResolutionOptions.size() || !window) {
        return;
    }

    const ResolutionOption& opt = kResolutionOptions[index];
    int cur_w = 0;
    int cur_h = 0;
    if (platform_) {
        platform_->get_window_size(cur_w, cur_h);
    }

    if (cur_w == opt.width && cur_h == opt.height) {
        hud_resolution_index_ = index;
        window_width_ = cur_w;
        window_height_ = cur_h;
        return;
    }

    std::cout << "[hud] resizing window to " << opt.width << " x " << opt.height << "\n";
    if (platform_) {
        platform_->set_window_size(opt.width, opt.height);
    }
    window_width_ = opt.width;
    window_height_ = opt.height;
    hud_resolution_index_ = index;
    hud_force_refresh_ = true;
}


bool VulkanApp::world_to_chunk_coords(const double pos[3], FaceChunkKey& key, int& lx, int& ly, int& lz, Int3& voxel_out) const {
    const PlanetConfig& cfg = planet_cfg_;
    const double voxel_m = cfg.voxel_size_m;
    const double chunk_m = (double)Chunk64::N * voxel_m;
    double px = pos[0];
    double py = pos[1];
    double pz = pos[2];
    double radius = std::sqrt(px * px + py * py + pz * pz);
    if (radius <= 0.0) return false;
    Float3 dir = wf::normalize(Float3{static_cast<float>(px), static_cast<float>(py), static_cast<float>(pz)});
    int face = 0; float u = 0.0f, v = 0.0f;
    if (!face_uv_from_direction(dir, face, u, v)) return false;
    Float3 right, up, forward; face_basis(face, right, up, forward);
    double s = px * right.x + py * right.y + pz * right.z;
    double t = px * up.x    + py * up.y    + pz * up.z;
    std::int64_t i = (std::int64_t)std::floor(s / chunk_m);
    std::int64_t j = (std::int64_t)std::floor(t / chunk_m);
    std::int64_t k = (std::int64_t)std::floor(radius / chunk_m);
    key = FaceChunkKey{ face, i, j, k };

    double s_local = s - (double)i * chunk_m;
    double t_local = t - (double)j * chunk_m;
    double r_local = radius - (double)k * chunk_m;
    int xi = (int)std::floor(s_local / voxel_m);
    int yi = (int)std::floor(t_local / voxel_m);
    int zi = (int)std::floor(r_local / voxel_m);
    if (xi < 0 || xi >= Chunk64::N || yi < 0 || yi >= Chunk64::N || zi < 0 || zi >= Chunk64::N) return false;
    lx = xi; ly = yi; lz = zi;
    voxel_out = Int3{ (i64)std::llround(px / voxel_m),
                      (i64)std::llround(py / voxel_m),
                      (i64)std::llround(pz / voxel_m) };
    return true;
}

bool VulkanApp::pick_voxel(VoxelHit& solid_hit, VoxelHit& empty_before) {
    const PlanetConfig& cfg = planet_cfg_;
    float cyaw = std::cos(cam_yaw_);
    float syaw = std::sin(cam_yaw_);
    float cp = std::cos(cam_pitch_);
    float sp = std::sin(cam_pitch_);
    Float3 dir = wf::normalize(Float3{cp * cyaw, sp, cp * syaw});
    double step = cfg.voxel_size_m * 0.5;
    if (step <= 0.0) step = 0.1;
    const double max_dist = edit_max_distance_m_;
    std::optional<VoxelHit> last_empty;

    for (double t = 0.0; t <= max_dist; t += step) {
        double pos[3] = {
            cam_pos_[0] + dir.x * t,
            cam_pos_[1] + dir.y * t,
            cam_pos_[2] + dir.z * t
        };

        FaceChunkKey key{};
        int lx = 0, ly = 0, lz = 0;
        Int3 voxel_idx{0,0,0};
        if (!world_to_chunk_coords(pos, key, lx, ly, lz, voxel_idx)) continue;

        uint16_t mat = MAT_AIR;
        bool found = streaming_.with_chunk(key, [&](const Chunk64& chunk) {
            mat = chunk.get_material(lx, ly, lz);
        });
        if (!found) continue;

        if (mat != MAT_AIR) {
            solid_hit.key = key;
            solid_hit.x = lx;
            solid_hit.y = ly;
            solid_hit.z = lz;
            solid_hit.voxel = voxel_idx;
            solid_hit.material = mat;
            solid_hit.world_pos[0] = pos[0];
            solid_hit.world_pos[1] = pos[1];
            solid_hit.world_pos[2] = pos[2];
            if (last_empty.has_value()) {
                empty_before = *last_empty;
            } else {
                empty_before = VoxelHit{};
                empty_before.key.face = -1;
            }
            return true;
        }

        VoxelHit empty{};
        empty.key = key;
        empty.x = lx;
        empty.y = ly;
        empty.z = lz;
        empty.voxel = voxel_idx;
        empty.material = MAT_AIR;
        empty.world_pos[0] = pos[0];
        empty.world_pos[1] = pos[1];
        empty.world_pos[2] = pos[2];
        last_empty = empty;
    }
    return false;
}

void VulkanApp::queue_chunk_remesh(const FaceChunkKey& key) {
    if (world_runtime_initialized_) {
        world_runtime_->queue_chunk_remesh(key);
    } else {
        streaming_.queue_remesh(key);
    }
}

void VulkanApp::process_pending_remeshes() {
    if (world_runtime_initialized_) {
        world_runtime_->process_pending_remeshes();
    }
}

bool VulkanApp::apply_voxel_edit(const VoxelHit& target, uint16_t new_material, int brush_dim) {
    if (world_runtime_initialized_) {
        return world_runtime_->apply_voxel_edit(target, new_material, brush_dim);
    }
    return false;
}

void VulkanApp::flush_dirty_chunk_deltas() {
    streaming_.flush_dirty_chunk_deltas();
}

void VulkanApp::apply_runtime_result(const WorldUpdateResult& result) {
    if (result.config_changed) {
        hud_force_refresh_ = true;
    }
}

bool VulkanApp::process_runtime_mesh_upload(const MeshUpload& upload) {
    RenderChunk rc;
    rc.chunk_renderer = &chunk_renderer_;
    rc.index_count = static_cast<uint32_t>(upload.mesh.indices.size());
    rc.vertex_count = static_cast<uint32_t>(upload.mesh.vertices.size());
    if (rc.vertex_count == 0 || rc.index_count == 0) {
        return false;
    }

    bool ok_upload = chunk_renderer_.upload_mesh(upload.mesh.vertices.data(), upload.mesh.vertices.size(),
                                                upload.mesh.indices.data(), upload.mesh.indices.size(),
                                                rc.first_index, rc.base_vertex);
    if (!ok_upload) {
        if (log_stream_) {
            std::cerr << "[stream] skip upload (pool full): face=" << upload.key.face
                      << " i=" << upload.key.i << " j=" << upload.key.j << " k=" << upload.key.k
                      << " vtx=" << upload.mesh.vertices.size() << " idx=" << upload.mesh.indices.size() << "\n";
        }
        return false;
    }

    rc.center[0] = upload.center[0];
    rc.center[1] = upload.center[1];
    rc.center[2] = upload.center[2];
    rc.radius = upload.radius;
    rc.key = upload.key;

    if (debug_chunk_keys_) {
        if (!upload.mesh.vertices.empty()) {
            const auto& v0 = upload.mesh.vertices.front();
            std::cout << "[mesh] key face=" << rc.key.face << " i=" << rc.key.i << " j=" << rc.key.j << " k=" << rc.key.k
                      << " v0=(" << v0.x << "," << v0.y << "," << v0.z << ")" << std::endl;
        }
        std::cout << "[mesh] center=(" << rc.center[0] << "," << rc.center[1] << "," << rc.center[2]
                  << ") radius=" << rc.radius << " idx=" << rc.index_count << " vtx=" << rc.vertex_count << std::endl;
    }

    bool replaced = false;
    RenderChunk* inserted_chunk = nullptr;
    for (size_t i = 0; i < render_chunks_.size(); ++i) {
        if (render_chunks_[i].key.face == rc.key.face &&
            render_chunks_[i].key.i == rc.key.i &&
            render_chunks_[i].key.j == rc.key.j &&
            render_chunks_[i].key.k == rc.key.k) {
            schedule_delete_chunk(std::move(render_chunks_[i]));
            render_chunks_[i] = std::move(rc);
            inserted_chunk = &render_chunks_[i];
            replaced = true;
            if (log_stream_) {
                std::cout << "[stream] replace: face=" << inserted_chunk->key.face
                          << " i=" << inserted_chunk->key.i << " j=" << inserted_chunk->key.j << " k=" << inserted_chunk->key.k
                          << " idx_count=" << inserted_chunk->index_count << " vtx_count=" << inserted_chunk->vertex_count
                          << " first_index=" << inserted_chunk->first_index << " base_vertex=" << inserted_chunk->base_vertex << "\n";
            }
            break;
        }
    }
    if (!replaced) {
        render_chunks_.push_back(std::move(rc));
        inserted_chunk = &render_chunks_.back();
        if (log_stream_) {
            std::cout << "[stream] add: face=" << inserted_chunk->key.face
                      << " i=" << inserted_chunk->key.i << " j=" << inserted_chunk->key.j << " k=" << inserted_chunk->key.k
                      << " idx_count=" << inserted_chunk->index_count << " vtx_count=" << inserted_chunk->vertex_count
                      << " first_index=" << inserted_chunk->first_index << " base_vertex=" << inserted_chunk->base_vertex << "\n";
        }
    }

    if (debug_chunk_keys_ && !debug_auto_aim_done_ && inserted_chunk) {
        Float3 eye{(float)cam_pos_[0], (float)cam_pos_[1], (float)cam_pos_[2]};
        Float3 center{inserted_chunk->center[0], inserted_chunk->center[1], inserted_chunk->center[2]};
        Float3 dir = wf::normalize(center - eye);
        cam_yaw_ = std::atan2(dir.z, dir.x);
        cam_pitch_ = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
        debug_auto_aim_done_ = true;
        std::cout << "[debug-aim] yaw=" << cam_yaw_ << " pitch=" << cam_pitch_ << "\n";
    }

    return true;
}

void VulkanApp::process_runtime_mesh_release(const FaceChunkKey& key) {
    for (size_t i = 0; i < render_chunks_.size(); ++i) {
        const auto& rc_key = render_chunks_[i].key;
        if (rc_key.face == key.face && rc_key.i == key.i && rc_key.j == key.j && rc_key.k == key.k) {
            streaming_.erase_chunk(rc_key);
            schedule_delete_chunk(std::move(render_chunks_[i]));
            render_chunks_.erase(render_chunks_.begin() + i);
            return;
        }
    }
}

void VulkanApp::draw_frame() {
    Renderer::FrameContext ctx = renderer_.begin_frame();
    if (trash_.size() != renderer_.frame_count()) {
        trash_.resize(renderer_.frame_count());
    }

    if (!ctx.acquired) {
        hud_ui_backend_.end_frame();
        if (renderer_.swapchain_needs_recreate()) {
            renderer_.recreate_swapchain();
            rebuild_swapchain_dependents();
        }
        return;
    }

    const size_t frame_index = ctx.frame_index % renderer_.frame_count();
    auto& deferred = trash_[frame_index];
    deferred.clear();

    process_pending_remeshes();
    drain_mesh_results();

    overlay_draw_slot_ = frame_index;
    const VkExtent2D swap_extent = renderer_.swapchain_extent();
    ui::ContextParams ui_params;
    ui_params.screen_width = static_cast<int>(swap_extent.width);
    ui_params.screen_height = static_cast<int>(swap_extent.height);
    ui_params.style.scale = hud_scale_;
    ui_params.style.enable_shadow = hud_shadow_enabled_;
    ui_params.style.shadow_offset_px = hud_shadow_offset_px_;
    ui_params.style.shadow_color = ui::Color{0.0f, 0.0f, 0.0f, 0.6f};
    hud_ui_context_.begin(ui_params);
    bool runtime_config_dirty = false;

    ui::TextDrawParams text_params;
    text_params.scale = 1.0f;
    text_params.color = ui::Color{1.0f, 1.0f, 1.0f, 1.0f};
    text_params.line_spacing_px = 4.0f;
    float text_height = 0.0f;
    if (!hud_text_.empty()) {
        text_height = ui::add_text_block(hud_ui_context_, hud_text_.c_str(), ui_params.screen_width, text_params);
    }

    ui::ButtonStyle button_style;
    button_style.text_scale = 1.0f;
    float button_y = text_params.origin_px.y + text_height + 8.0f;
    const float button_height = 18.0f;
    const float button_width = 136.0f;
    const float button_spacing = 4.0f;

    auto button_rect_at = [&](float y) {
        return ui::Rect{6.0f, y, button_width, button_height};
    };

    std::string button_label = std::string("Cull: ") + (cull_enabled_ ? "ON" : "OFF");
    if (ui::button(hud_ui_context_, hud_ui_backend_, ui::hash_id("hud.cull"), button_rect_at(button_y), button_label, button_style)) {
        cull_enabled_ = !cull_enabled_;
        hud_force_refresh_ = true;
        runtime_config_dirty = true;
    }
    button_y += button_height + button_spacing;

    std::string axes_label = std::string("Axes: ") + (debug_show_axes_ ? "ON" : "OFF");
    if (ui::button(hud_ui_context_, hud_ui_backend_, ui::hash_id("hud.axes"), button_rect_at(button_y), axes_label, button_style)) {
        debug_show_axes_ = !debug_show_axes_;
        hud_force_refresh_ = true;
    }
    button_y += button_height + button_spacing;

    std::string tri_label = std::string("Tri: ") + (debug_show_test_triangle_ ? "ON" : "OFF");
    if (ui::button(hud_ui_context_, hud_ui_backend_, ui::hash_id("hud.triangle"), button_rect_at(button_y), tri_label, button_style)) {
        debug_show_test_triangle_ = !debug_show_test_triangle_;
        hud_force_refresh_ = true;
    }
    button_y += button_height + button_spacing;

    if (ui::button(hud_ui_context_, hud_ui_backend_, ui::hash_id("hud.reload_config"), button_rect_at(button_y), "Reload Config", button_style)) {
        reload_config_from_disk();
    }
    button_y += button_height + button_spacing;

    if (ui::button(hud_ui_context_, hud_ui_backend_, ui::hash_id("hud.save_config"), button_rect_at(button_y), "Save Config", button_style)) {
        save_active_config();
    }
    button_y += button_height + button_spacing;

    std::string auto_label = std::string("Auto Reload: ") + (config_auto_reload_enabled_ ? "ON" : "OFF");
    if (ui::button(hud_ui_context_, hud_ui_backend_, ui::hash_id("hud.auto_reload"), button_rect_at(button_y), auto_label, button_style)) {
        config_auto_reload_enabled_ = !config_auto_reload_enabled_;
        hud_force_refresh_ = true;
    }
    button_y += button_height + button_spacing;

    std::array<std::string_view, kResolutionOptions.size()> resolution_labels{};
    for (std::size_t i = 0; i < kResolutionOptions.size(); ++i) {
        resolution_labels[i] = kResolutionOptions[i].label;
    }

    ui::Rect dropdown_rect{6.0f, button_y, button_width, button_height};
    ui::DropdownResult resolution_dropdown = ui::dropdown(
        hud_ui_context_,
        hud_ui_backend_,
        ui::hash_id("hud.resolution"),
        dropdown_rect,
        std::span<const std::string_view>(resolution_labels.data(), resolution_labels.size()),
        std::min(hud_resolution_index_, kResolutionOptions.size() - 1));

    if (resolution_dropdown.selection_changed) {
        apply_resolution_option(resolution_dropdown.selected_index);
    } else {
        hud_resolution_index_ = std::min(resolution_dropdown.selected_index, kResolutionOptions.size() - 1);
    }

    const float hud_scale = (ui_params.style.scale > 0.0f) ? ui_params.style.scale : 1.0f;
    auto unscale = [&](float px) { return px / hud_scale; };

    const float tool_slot_px = 72.0f;
    const float tool_slot_spacing_px = 16.0f;
    const float tool_bottom_margin_px = 32.0f;
    const float tool_total_width_px = tool_slot_px * 2.0f + tool_slot_spacing_px;
    const float tool_start_x_px = (ui_params.screen_width - tool_total_width_px) * 0.5f;
    const float tool_y_px = std::max(0.0f, ui_params.screen_height - tool_bottom_margin_px - tool_slot_px);

    ui::SelectableStyle tool_style;
    tool_style.text_scale = 0.9f;
    tool_style.padding_px = 8.0f;

    auto tool_rect_at = [&](int index) {
        float x_px = tool_start_x_px + static_cast<float>(index) * (tool_slot_px + tool_slot_spacing_px);
        return ui::Rect{unscale(x_px), unscale(tool_y_px), unscale(tool_slot_px), unscale(tool_slot_px)};
    };

    bool small_selected = (selected_tool_ == ToolSelection::SmallShovel);
    bool clicked_small = ui::selectable(hud_ui_context_,
                                        hud_ui_backend_,
                                        ui::hash_id("hud.tool.small"),
                                        tool_rect_at(0),
                                        "Small\nShovel",
                                        small_selected,
                                        tool_style);
    if (clicked_small) {
        selected_tool_ = small_selected ? ToolSelection::None : ToolSelection::SmallShovel;
        hud_force_refresh_ = true;
    }

    bool large_selected = (selected_tool_ == ToolSelection::LargeShovel);
    bool clicked_large = ui::selectable(hud_ui_context_,
                                        hud_ui_backend_,
                                        ui::hash_id("hud.tool.large"),
                                        tool_rect_at(1),
                                        "Big\nShovel",
                                        large_selected,
                                        tool_style);
    if (clicked_large) {
        selected_tool_ = large_selected ? ToolSelection::None : ToolSelection::LargeShovel;
        hud_force_refresh_ = true;
    }

    ui::Vec2 crosshair_center{ui_params.screen_width * 0.5f, ui_params.screen_height * 0.5f};
    ui::CrosshairStyle crosshair_style;
    crosshair_style.color = ui::Color{1.0f, 1.0f, 1.0f, 0.9f};
    crosshair_style.arm_length_px = 9.0f;
    crosshair_style.center_gap_px = 6.0f;
    crosshair_style.arm_thickness_px = 2.0f;
    ui::crosshair(hud_ui_context_, crosshair_center, crosshair_style);

    ui::UIDrawData draw_data = hud_ui_context_.end();
    overlay_.upload_draw_data(overlay_draw_slot_, draw_data);

    vkResetCommandBuffer(ctx.command_buffer, 0);
    record_command_buffer(ctx);

    renderer_.submit_frame(ctx);
    renderer_.present_frame(ctx);

    hud_ui_backend_.end_frame();

    if (world_runtime_initialized_ && runtime_config_dirty) {
        world_runtime_->apply_config(snapshot_config());
    }

    if (renderer_.swapchain_needs_recreate()) {
        renderer_.recreate_swapchain();
        rebuild_swapchain_dependents();
    }
}

void VulkanApp::recreate_swapchain() {
    renderer_.wait_idle();
    renderer_.recreate_swapchain();
    rebuild_swapchain_dependents();
}

void VulkanApp::rebuild_swapchain_dependents() {
    VkDevice device = renderer_.device();
    if (!device) {
        return;
    }

    trash_.resize(renderer_.frame_count());

#include "wf_config.h"
    const VkPhysicalDevice physical = renderer_.physical_device();
    const VkRenderPass render_pass = renderer_.render_pass();
    const VkExtent2D extent = renderer_.swapchain_extent();

    framebuffer_width_ = static_cast<int>(extent.width);
    framebuffer_height_ = static_cast<int>(extent.height);
    hud_resolution_index_ = find_resolution_index(framebuffer_width_, framebuffer_height_);

    pipeline_triangle_.reset();
    pipeline_layout_.reset();
    destroy_debug_axes_pipeline();

    if (!overlay_initialized_) {
        overlay_.init(physical, device, render_pass, extent, WF_SHADER_DIR);
        overlay_initialized_ = true;
    } else {
        overlay_.recreate_swapchain(render_pass, extent, WF_SHADER_DIR);
    }

    if (!chunk_renderer_initialized_) {
        chunk_renderer_.init(physical, device, render_pass, extent, WF_SHADER_DIR);
        chunk_renderer_initialized_ = true;
    } else {
        chunk_renderer_.recreate(render_pass, extent, WF_SHADER_DIR);
    }
    chunk_renderer_.set_device_local(device_local_enabled_);
    chunk_renderer_.set_transfer_context(renderer_.graphics_queue_family(), renderer_.graphics_queue());
    chunk_renderer_.set_pool_caps_bytes(static_cast<VkDeviceSize>(pool_vtx_mb_) * 1024ull * 1024ull,
                                        static_cast<VkDeviceSize>(pool_idx_mb_) * 1024ull * 1024ull);
    chunk_renderer_.set_logging(log_pool_);

    create_graphics_pipeline();
    hud_force_refresh_ = true;
}

VkShaderModule VulkanApp::load_shader_module(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    std::vector<std::string> fallbacks = {
        std::string("build/shaders/") + base,
        std::string("shaders/") + base,
        std::string("shaders_build/") + base
    };
    VkDevice device = renderer_.device();
    if (!device) {
        return VK_NULL_HANDLE;
    }
    return wf::vk::load_shader_module(device, path, fallbacks);
}

void VulkanApp::create_graphics_pipeline() {
    VkDevice device = renderer_.device();
    if (!device) {
        return;
    }
    VkRenderPass render_pass = renderer_.render_pass();
    if (render_pass == VK_NULL_HANDLE) {
        return;
    }
    const VkExtent2D swap_extent = renderer_.swapchain_extent();

#include "wf_config.h"
    pipeline_triangle_.reset();
    pipeline_layout_.reset();

    const std::string base = std::string(WF_SHADER_DIR);
    const std::string vsPath = base + "/triangle.vert.spv";
    const std::string fsPath = base + "/triangle.frag.spv";
    wf::vk::UniqueShaderModule vs(device, load_shader_module(vsPath));
    wf::vk::UniqueShaderModule fs(device, load_shader_module(fsPath));
    if (!vs || !fs) {
        std::cout << "[info] Shaders not found (" << vsPath << ", " << fsPath << "). Triangle disabled." << std::endl;
        return;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs.get();
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs.get();
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = static_cast<float>(swap_extent.height);
    vp.width = static_cast<float>(swap_extent.width);
    vp.height = -static_cast<float>(swap_extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    VkRect2D sc{};
    sc.offset = {0, 0};
    sc.extent = swap_extent;
    VkPipelineViewportStateCreateInfo vpstate{};
    vpstate.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpstate.viewportCount = 1;
    vpstate.pViewports = &vp;
    vpstate.scissorCount = 1;
    vpstate.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT; cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPipelineLayout new_layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device, &plci, nullptr, &new_layout) != VK_SUCCESS) {
        return;
    }
    pipeline_layout_.reset(device, new_layout);

    VkGraphicsPipelineCreateInfo gpi{};
    gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vpstate;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = nullptr;
    gpi.pColorBlendState = &cb;
    gpi.layout = pipeline_layout_.get();
    gpi.renderPass = render_pass;
    gpi.subpass = 0;
    VkPipeline new_pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &new_pipeline) != VK_SUCCESS) {
        std::cerr << "Failed to create graphics pipeline.\n";
        pipeline_layout_.reset();
        return;
    }
    pipeline_triangle_.reset(device, new_pipeline);

    create_debug_axes_pipeline();
}

// legacy chunk pipeline removed (ChunkRenderer owns chunk graphics pipeline)
// removed overlay pipeline (handled by OverlayRenderer)


// removed overlay buffer update (handled by OverlayRenderer)

void VulkanApp::create_debug_axes_buffer() {
    if (debug_axes_vbo_) return;
    VkDevice device = renderer_.device();
    VkPhysicalDevice physical = renderer_.physical_device();
    if (!device || physical == VK_NULL_HANDLE) return;
    const float axis_len = 1500.0f;
    const DebugAxisVertex verts[] = {
        {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
        {{axis_len, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
        {{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {{0.0f, axis_len, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.0f, 0.0f, axis_len}, {0.0f, 0.0f, 1.0f}},
    };
    VkDeviceSize bytes = sizeof(verts);
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    wf::vk::create_buffer(physical, device, bytes,
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          buf, mem);
    debug_axes_vbo_.reset(device, buf);
    debug_axes_vbo_mem_.reset(device, mem);
    wf::vk::upload_host_visible(device, debug_axes_vbo_mem_.get(), bytes, verts, 0);
    debug_axes_vertex_count_ = static_cast<uint32_t>(sizeof(verts) / sizeof(verts[0]));
}

void VulkanApp::destroy_debug_axes_buffer() {
    debug_axes_vbo_.reset();
    debug_axes_vbo_mem_.reset();
    debug_axes_vertex_count_ = 0;
}

void VulkanApp::create_debug_axes_pipeline() {
    destroy_debug_axes_pipeline();
    VkDevice device = renderer_.device();
    VkRenderPass render_pass = renderer_.render_pass();
    if (!device || render_pass == VK_NULL_HANDLE) return;
    create_debug_axes_buffer();

#include "wf_config.h"
    const std::string base = std::string(WF_SHADER_DIR);
    const std::string vsPath = base + "/debug_axes.vert.spv";
    const std::string fsPath = base + "/debug_axes.frag.spv";
    wf::vk::UniqueShaderModule vs(device, load_shader_module(vsPath));
    wf::vk::UniqueShaderModule fs(device, load_shader_module(fsPath));
    if (!vs || !fs) {
        std::cout << "[info] Debug axis shaders not found; axis gizmo disabled." << std::endl;
        return;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs.get();
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs.get();
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(DebugAxisVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = offsetof(DebugAxisVertex, pos);
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset = offsetof(DebugAxisVertex, color);
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

    const VkExtent2D swap_extent = renderer_.swapchain_extent();
    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = static_cast<float>(swap_extent.height);
    vp.width = static_cast<float>(swap_extent.width);
    vp.height = -static_cast<float>(swap_extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    VkRect2D sc{};
    sc.offset = {0, 0};
    sc.extent = swap_extent;
    VkPipelineViewportStateCreateInfo vpstate{};
    vpstate.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpstate.viewportCount = 1;
    vpstate.pViewports = &vp;
    vpstate.scissorCount = 1;
    vpstate.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_LINE;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 2.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(float) * 16;
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VkPipelineLayout dbg_layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device, &plci, nullptr, &dbg_layout) != VK_SUCCESS) {
        return;
    }
    debug_axes_layout_.reset(device, dbg_layout);

    VkGraphicsPipelineCreateInfo gpi{};
    gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vpstate;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &ds;
    gpi.pColorBlendState = &cb;
    gpi.layout = debug_axes_layout_.get();
    gpi.renderPass = render_pass;
    gpi.subpass = 0;
    VkPipeline dbg_pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, nullptr, &dbg_pipeline) != VK_SUCCESS) {
        debug_axes_layout_.reset();
        std::cerr << "Failed to create debug axes pipeline.\n";
        return;
    }

    debug_axes_pipeline_.reset(device, dbg_pipeline);
}

void VulkanApp::destroy_debug_axes_pipeline() {
    debug_axes_pipeline_.reset();
    debug_axes_layout_.reset();
}

void VulkanApp::create_compute_pipeline() {
    VkDevice device = renderer_.device();
    if (!device) return;
    // Load no-op compute shader
    const std::string base = std::string(WF_SHADER_DIR);
    const std::string csPath = base + "/noop.comp.spv";
    pipeline_compute_.reset();
    pipeline_layout_compute_.reset();
    wf::vk::UniqueShaderModule cs(device, load_shader_module(csPath));
    if (!cs) {
        std::cout << "[info] Compute shader not found (" << csPath << "). Compute disabled." << std::endl;
        return;
    }
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = cs.get();
    stage.pName = "main";

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPipelineLayout new_layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device, &plci, nullptr, &new_layout) != VK_SUCCESS) {
        return;
    }
    pipeline_layout_compute_.reset(device, new_layout);

    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage = stage;
    ci.layout = pipeline_layout_compute_.get();
    VkPipeline new_pipeline = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &new_pipeline) != VK_SUCCESS) {
        std::cerr << "Failed to create compute pipeline." << std::endl;
        pipeline_layout_compute_.reset();
        return;
    }
    pipeline_compute_.reset(device, new_pipeline);
}

void VulkanApp::profile_append_csv(const std::string& line) {
    if (!profile_csv_enabled_) return;
    std::lock_guard<std::mutex> lk(profile_mutex_);
    std::ofstream out;
    out.open(profile_csv_path_, profile_header_written_ ? (std::ios::app) : (std::ios::out));
    if (!out.good()) return;
    if (!profile_header_written_) {
        out << "event,time_s,items,meshed,gen_ms,mesh_ms,total_or_frame_ms" << '\n';
        profile_header_written_ = true;
    }
    out << line;
}

void VulkanApp::schedule_delete_chunk(RenderChunk&& rc) {
    // Defer destruction to the next frame slot to guarantee GPU is done using previous submissions.
    size_t frame_count = renderer_.frame_count();
    if (trash_.size() != frame_count) {
        trash_.resize(frame_count);
    }
    size_t slot = (renderer_.current_frame() + 1) % frame_count;
    trash_[slot].push_back(std::move(rc));
}


void VulkanApp::drain_mesh_results() {
    if (!world_runtime_initialized_) {
        return;
    }

    auto uploads = world_runtime_->pending_mesh_uploads();
    auto releases = world_runtime_->pending_mesh_releases();

    int uploaded = 0;
    auto t0 = std::chrono::steady_clock::now();

    std::size_t uploads_processed = 0;
    std::size_t max_uploads = uploads_per_frame_limit_ > 0
                                  ? static_cast<std::size_t>(uploads_per_frame_limit_)
                                  : uploads.size();
    for (; uploads_processed < uploads.size() && uploads_processed < max_uploads; ++uploads_processed) {
        if (process_runtime_mesh_upload(uploads[uploads_processed])) {
            ++uploaded;
        }
    }

    std::size_t releases_processed = releases.size();
    for (const auto& key : releases) {
        process_runtime_mesh_release(key);
    }

    world_runtime_->consume_mesh_transfer_queues(uploads_processed, releases_processed);

    auto runtime_allows = world_runtime_->active_allow_regions();
    if (!runtime_allows.empty()) {
        std::vector<AllowRegion> allows;
        allows.reserve(runtime_allows.size());
        for (const auto& region : runtime_allows) {
            allows.push_back(AllowRegion{region.face, region.ci, region.cj, region.ck, region.span, region.k_down, region.k_up});
        }
        prune_chunks_multi(allows);
    }

    auto t1 = std::chrono::steady_clock::now();
    if (uploaded > 0) {
        last_upload_count_ = uploaded;
        last_upload_ms_ = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (upload_ms_avg_ <= 0.0) {
            upload_ms_avg_ = last_upload_ms_;
        } else {
            upload_ms_avg_ = upload_ms_avg_ * 0.8 + last_upload_ms_ * 0.2;
        }
        if (profile_csv_enabled_) {
            double tsec = std::chrono::duration<double>(t1 - app_start_tp_).count();
            char line[128];
            std::snprintf(line, sizeof(line), "upload,%.3f,%d,%.3f\n", tsec, uploaded, last_upload_ms_);
            profile_append_csv(line);
        }
    } else {
        last_upload_count_ = 0;
        last_upload_ms_ = 0.0;
    }
}

void VulkanApp::prune_chunks_outside(int face, std::int64_t ci, std::int64_t cj, int span) {
    for (size_t i = 0; i < render_chunks_.size();) {
        const auto& rk = render_chunks_[i].key;
        if (rk.face != face || std::llabs(rk.i - ci) > span || std::llabs(rk.j - cj) > span) {
            if (log_stream_) {
                std::cout << "[stream] prune: face=" << rk.face << " i=" << rk.i << " j=" << rk.j << " k=" << rk.k
                          << " first_index=" << render_chunks_[i].first_index
                          << " base_vertex=" << render_chunks_[i].base_vertex
                          << " idx_count=" << render_chunks_[i].index_count
                          << " vtx_count=" << render_chunks_[i].vertex_count << "\n";
            }
            streaming_.erase_chunk(rk);
            // Defer deletion/free; chunk might still be referenced by commands submitted last frame
            schedule_delete_chunk(std::move(render_chunks_[i]));
            render_chunks_.erase(render_chunks_.begin() + i);
        } else {
            ++i;
        }
    }
}

void VulkanApp::prune_chunks_multi(const std::vector<AllowRegion>& allows) {
    auto inside_any = [&](const FaceChunkKey& k) -> bool {
        for (const auto& a : allows) {
            if (k.face != a.face) continue;
            if (std::llabs(k.i - a.ci) > a.span) continue;
            if (std::llabs(k.j - a.cj) > a.span) continue;
            std::int64_t kmin = a.ck - a.k_down;
            std::int64_t kmax = a.ck + a.k_up;
            if (k.k < kmin || k.k > kmax) continue;
            return true;
        }
        return false;
    };
    for (size_t i = 0; i < render_chunks_.size();) {
        auto& rc = render_chunks_[i];
        if (!inside_any(rc.key)) {
            if (log_stream_) {
                std::cout << "[stream] prune: face=" << rc.key.face << " i=" << rc.key.i << " j=" << rc.key.j << " k=" << rc.key.k
                          << " first_index=" << rc.first_index
                          << " base_vertex=" << rc.base_vertex
                          << " idx_count=" << rc.index_count
                          << " vtx_count=" << rc.vertex_count << "\n";
            }
            streaming_.erase_chunk(rc.key);
            schedule_delete_chunk(std::move(rc));
            render_chunks_.erase(render_chunks_.begin() + i);
        } else {
            ++i;
        }
    }
}

}
