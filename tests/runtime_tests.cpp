#include "consumer/runtime.hpp"
#include <iostream>
int main() {
    try {
        runtime_consumer::run();
        std::cout << "PASS shared-world scene phases, physics, audio and transitions\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
