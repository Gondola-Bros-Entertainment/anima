// Pins how scene work grows with the objects it touches, through the bytes it allocates. This
// executable replaces the global allocation functions, so it counts every allocation the engine
// makes during a measured operation.
#include <anima/scene.hpp>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdlib>
#include <new>
#include <vector>

namespace {
std::size_t counted_allocation_bytes = 0;
bool counting_allocations = false;

// Bytes allocated while @p operation runs.
template <class Operation> std::size_t allocated_by(Operation &&operation) {
    counted_allocation_bytes = 0;
    counting_allocations = true;
    operation();
    counting_allocations = false;
    return counted_allocation_bytes;
}

// Bytes allocated while @p count new objects are attached to one parent.
std::size_t wide_attach_bytes(std::size_t count) {
    anima::Scene scene;
    auto parent = scene.create();
    std::vector<anima::GameObject> children;
    for (std::size_t i = 0; i < count; ++i)
        children.push_back(scene.create());
    return allocated_by([&] {
        for (auto &child : children)
            child.set_parent(parent);
    });
}
} // namespace

void *operator new(std::size_t bytes) {
    if (counting_allocations)
        counted_allocation_bytes += bytes;
    if (void *allocation = std::malloc(bytes ? bytes : 1))
        return allocation;
    throw std::bad_alloc();
}
void operator delete(void *allocation) noexcept { std::free(allocation); }
void operator delete(void *allocation, std::size_t) noexcept { std::free(allocation); }

TEST_CASE("Attaching children allocates no more per child as their parent grows") {
    // Reallocating the parent's child list on each attach would allocate 16 times as many bytes
    // per child at the larger count.
    const auto small = wide_attach_bytes(256), large = wide_attach_bytes(4096);
    CHECK(large <= 16 * small);
}
