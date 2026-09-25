#include "consumer/lifecycle.hpp"
#include <iostream>
int main() {
    try {
        lifecycle_test::run();
        std::cout << "PASS inherited activation, lifecycle, scheduling and persistence\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
