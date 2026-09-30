#include "allocation_counter.hpp"

#include <cstdlib>
#include <new>

namespace allocation_counter {
std::size_t bytes = 0;
bool counting = false;
std::size_t allowed = 0;
bool failing = false;
} // namespace allocation_counter

void *operator new(std::size_t size) {
    if (allocation_counter::counting)
        allocation_counter::bytes += size;
    if (allocation_counter::failing) {
        if (allocation_counter::allowed == 0)
            throw std::bad_alloc();
        --allocation_counter::allowed;
    }
    if (void *allocation = std::malloc(size ? size : 1))
        return allocation;
    throw std::bad_alloc();
}
void operator delete(void *allocation) noexcept { std::free(allocation); }
void operator delete(void *allocation, std::size_t) noexcept { std::free(allocation); }
