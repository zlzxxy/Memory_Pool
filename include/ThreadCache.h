#pragma once
#include "Common.h"
#include <array>
#include <cstdint>

namespace memoryPool {

class ThreadCache {
public:
    static ThreadCache* getInstance() {
        static thread_local ThreadCache instance;
        return &instance;
    }

    void* allocate(size_t size);
    void deallocate(void* ptr, size_t size);

private:
    ThreadCache() = default;

    void* fetchFromCentralCache(size_t index);
    void returnToCentralCache(size_t index);

    static size_t getBatchNum(size_t size);
    static size_t getHighWaterMark(size_t size);

private:
    std::array<void*, FREE_LIST_SIZE> freeList_{};
    std::array<uint32_t, FREE_LIST_SIZE> freeListSize_{};
};

}