#include "consumer/references.hpp"
#include <iostream>
int main() {
    try {
        references_test::run();
        std::cout << "PASS stable scene keys, references, prefab remapping and rollback\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
