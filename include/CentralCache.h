#pragma once
#include "Common.h"
#include <array>
#include <mutex>

namespace memoryPool {

class CentralCache {
public:
    static CentralCache& getInstance() {
        static CentralCache instance;
        return instance;
    }

    void* fetchRange(size_t index, size_t& actualNum);
    void returnRange(void* start, size_t blockCount, size_t index);

private:
    CentralCache() = default;
    CentralCache(const CentralCache&) = delete;
    CentralCache& operator=(const CentralCache&) = delete;

    void* fetchFromPageCache(size_t size);

private:
    std::array<void*, FREE_LIST_SIZE> centralFreeList_{};
    std::array<std::mutex, FREE_LIST_SIZE> locks_{};
};

}