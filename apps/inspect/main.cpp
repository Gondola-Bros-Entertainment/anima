#include <anima/assets/mesh_snapshot.hpp>
#include <iostream>
int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: anima_inspect FILE.glb\n";
        return 1;
    }
    try {
        anima::print_mesh_report(anima::load_glb(argv[1]));
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "GLB: " << error.what() << '\n';
        return 1;
    }
}
