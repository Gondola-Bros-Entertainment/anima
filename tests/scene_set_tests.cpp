#include "consumer/scene_set.hpp"
#include <iostream>
int main() {
    try {
        scene_set_test::run();
        std::cout << "PASS additive scene ownership, replacement, namespaces and unload\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
