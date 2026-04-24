#include "ThreadCache.h"
#include "CentralCache.h"

#include <cstdlib>

namespace memoryPool {

namespace {

inline size_t getBatchNumBySize(size_t size) {
    constexpr size_t TARGET_BYTES = 64 * 1024;
    constexpr size_t MIN_BATCH = 8;
    constexpr size_t MAX_BATCH = 512;

    size_t batch = TARGET_BYTES / (size ? size : ALIGNMENT);
    if (batch < MIN_BATCH) batch = MIN_BATCH;
    if (batch > MAX_BATCH) batch = MAX_BATCH;
    return batch;
}

inline size_t getHighWaterMarkBySize(size_t size) {
    const size_t batch = getBatchNumBySize(size);

    if (size <= 64) {
        return batch * 8;
    }
    if (size <= 256) {
        return batch * 4;
    }
    return batch * 2;
}

}

void* ThreadCache::allocate(size_t size) {
    if (size == 0) {
        size = ALIGNMENT;
    }

    if (size > MAX_BYTES) {
        return std::malloc(size);
    }

    const size_t index = SizeClass::getIndex(size);
    void* ptr = freeList_[index];    // 获得链表的头节点

    // 头删法
    if (ptr) {
        // *reinterpret_cast<void**>(ptr): 把ptr这块内存的前8个字节解释成void* next
        freeList_[index] = *reinterpret_cast<void**>(ptr);
        freeListSize_[index]--;
        return ptr;
    }

    return fetchFromCentralCache(index);
}

void* ThreadCache::fetchFromCentralCache(size_t index) {
    size_t actualNum = 0;
    void* start = CentralCache::getInstance().fetchRange(index, actualNum);
    if (!start || actualNum == 0) {
        return nullptr;
    }

    void* result = start;

    if (actualNum == 1) {
        freeList_[index] = nullptr;
        return result;
    }

    freeList_[index] = *reinterpret_cast<void**>(start);
    freeListSize_[index] += static_cast<uint32_t>(actualNum - 1);
    return result;
}

void ThreadCache::deallocate(void* ptr, size_t size) {
    if (!ptr) {
        return;
    }

    if (size > MAX_BYTES) {
        std::free(ptr);
        return;
    }

    const size_t index = SizeClass::getIndex(size);
    // 头插法
    *reinterpret_cast<void**>(ptr) = freeList_[index];
    freeList_[index] = ptr;

    const uint32_t newSize = freeListSize_[index]++;
    const size_t blockSize = SizeClass::indexToSize(index);

    // 如果线程缓存里的空闲块太多，就不能一直留在线程里，getHighWaterMark是高水位线
    if (newSize > getHighWaterMark(blockSize)) {
        returnToCentralCache(index);
    }
}

void ThreadCache::returnToCentralCache(size_t index) {
    const uint32_t batchNum = freeListSize_[index];
    if (batchNum <= 1) {
        return;
    }

    const size_t blockSize = SizeClass::indexToSize(index);
    const size_t keepNum = getBatchNum(blockSize);

    if (batchNum <= keepNum) {
        return;
    }

    const size_t returnNum = batchNum - keepNum;

    void* head = freeList_[index];
    void* split = head;

    for (size_t i = 1; i < keepNum; ++i) {
        split = *reinterpret_cast<void**>(split);
    }

    void* returnHead = *reinterpret_cast<void**>(split);
    *reinterpret_cast<void**>(split) = nullptr;

    freeList_[index] = head;
    freeListSize_[index] = static_cast<uint32_t>(keepNum);

    CentralCache::getInstance().returnRange(returnHead, returnNum, index);
}

size_t ThreadCache::getBatchNum(size_t size) {
    return getBatchNumBySize(SizeClass::roundUp(size));
}

size_t ThreadCache::getHighWaterMark(size_t size) {
    return getHighWaterMarkBySize(SizeClass::roundUp(size));
}

}