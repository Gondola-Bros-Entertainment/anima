#include "consumer/scene_objects.hpp"
#include <iostream>
int main() {
    try {
        scene_objects_test::run();
        std::cout << "PASS scene objects, lifetime, terrain, mesh preparation and independent animation\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
