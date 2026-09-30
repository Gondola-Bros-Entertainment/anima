#pragma once
#include <cstddef>

// State of the replacement global allocation functions in allocation_counter.cpp, which count, and can
// fail, every allocation of the executable that links them. They have their own translation unit so that
// no caller inlines them: GCC reports an inlined operator delete's free() of memory from a call to
// operator new as a mismatched deallocation.
namespace allocation_counter {
// Bytes allocated while counting.
extern std::size_t bytes;
extern bool counting;
// While failing, each allocation beyond the next `allowed` throws std::bad_alloc.
extern std::size_t allowed;
extern bool failing;
} // namespace allocation_counter
