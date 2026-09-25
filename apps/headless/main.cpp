#include <anima/core/fixed_step.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>

int main() {
    using namespace std::chrono_literals;
    anima::FixedStepClock clock{10ms};
    std::uint64_t ticks = 0;
    for (int frame = 0; frame < 100; ++frame) {
        ticks += clock.advance(10ms).steps;
    }
    std::cout << "Anima headless: " << ticks << " fixed simulation ticks; no window or graphics dependency\n";
    return ticks == 100 ? 0 : 1;
}
