#include "viewer_application.hpp"
#include <SDL3/SDL_main.h>
#include <iostream>
int main(int argc, char **argv) {
    try {
        auto options = anima::viewer::parse_viewer_options(argc, argv);
        if (options.help) {
            std::cout << anima::viewer::viewer_usage();
            return 0;
        }
        return anima::viewer::run_viewer(std::move(options));
    } catch (const std::exception &error) {
        std::cerr << "Anima: " << error.what() << '\n';
        return 1;
    }
}
