#pragma once
#include "Common.h"
#include <array>
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
    void returnToCentralCache(void* start, size_t size);
    size_t getBatchNum(size_t size);
    bool shouldReturnToCentralcache(size_t index);

private:
    //定长数组，里面存放FREE_LIST_SIZE个对象
    std::array<void*, FREE_LIST_SIZE> freeList_{};
    std::array<size_t, FREE_LIST_SIZE> freeListSize_{};
};

}