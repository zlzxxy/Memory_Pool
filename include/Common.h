#pragma once
#include <cstddef>

namespace memoryPool
{
constexpr size_t SPAN_PAGES = 8;
constexpr size_t ALIGNMENT = 8;
constexpr size_t MAX_BYTES = 256 * 1024;
constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT;

class SizeClass
{
public:
    static inline size_t roundUp(size_t bytes)
    {
        return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    }

    static inline size_t getIndex(size_t bytes)
    {
        return (bytes <= ALIGNMENT) ? 0 : ((bytes - 1) >> 3);
    }

    static inline size_t indexToSize(size_t index)
    {
        return (index + 1) << 3;
    }
};

}