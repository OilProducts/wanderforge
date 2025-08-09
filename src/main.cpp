#include "vk_app.h"
#include "planet.h"

int main() {
    // Phase 2: quick base sampler sanity log (single point)
    wf::PlanetConfig cfg;
    wf::Int3 center{0, (wf::i64)(cfg.radius_m / cfg.voxel_size_m), 0};
    auto s = wf::sample_base(cfg, center);
    (void)s; // quiet unused warning
    wf::VulkanApp app;
    app.run();
    return 0;
}
