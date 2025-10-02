#include "vk_app.h"
#include "CLI11.hpp"

#include <string>

int main(int argc, char** argv) {
    CLI::App cli{"Wanderforge"};
    std::string config_path_cli;
    auto opt_config = cli.add_option("-c,--config", config_path_cli,
        "Path to wanderforge.cfg file (defaults to wanderforge.cfg in current directory)");
    CLI11_PARSE(cli, argc, argv);

    wf::VulkanApp app;
    if (opt_config->count() > 0) {
        app.set_config_path(config_path_cli);
    }
    app.run();
    return 0;
}
