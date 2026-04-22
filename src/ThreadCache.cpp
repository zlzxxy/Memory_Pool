#include "ThreadCache.h"
#include "CentralCache.h"

namespace memoryPool {
void* ThreadCache::allocate(size_t size) {
    if (size == 0) {
        size = ALIGNMENT;
    }

    if (size > MAX_BYTES) {
        return malloc(size);
    }

    size_t index = SizeClass::getIndex(size);

    void* ptr = freeList_[index];
    if (ptr != nullptr) {
        void* next = *reinterpret_cast<void**>(ptr);
        freeList_[index] = next;
        freeListSize_[index]--;
        return ptr;
    }

    return fetchFromCentralCache(index);
}

void* ThreadCache::fetchFromCentralCache(size_t index) {
    void* start = CentralCache::getInstance().fetchRange(index);
    if (!start) return nullptr;

    //这里的start表示这一批块的第一个节点
    void* result = start;
    freeList_[index] = *reinterpret_cast<void**>(start);

    size_t batchNum = 0;
    void* current = start;

    while (current != nullptr) {
        batchNum++;
        current = *reinterpret_cast<void**>(current);
    }

    freeListSize_[index] += batchNum - 1;

    return result;
}

void ThreadCache::deallocate(void* ptr, size_t size) {
    if (size > MAX_BYTES) {
        free(ptr);
        return;
    }

    size_t index = SizeClass::getIndex(size);

    *reinterpret_cast<void**>(ptr) = freeList_[index];
    freeList_[index] = ptr;

    freeListSize_[index]++;

    if (shouldReturnToCentralcache(index)) {
        returnToCentralCache(freeList_[index], size);
    }
}

void ThreadCache::returnToCentralCache(void* start, size_t size) {
    size_t index = SizeClass::getIndex(size);

    size_t alignedSize = SizeClass::roundUp(size);

    size_t batchNum = freeListSize_[index];
    if (batchNum <= 1) return;

    size_t keepNum = std::max(batchNum / 4, size_t(1));
    size_t returnNum = batchNum - keepNum;

    char* current = static_cast<char*>(start);
    char* splitNode = current;
    for (size_t i = 0; i < keepNum - 1; i++) {
        splitNode = reinterpret_cast<char*>(*reinterpret_cast<void**>(splitNode));
        if (splitNode == nullptr) {
            returnNum = batchNum - (i + 1);
            break;
        }
    }
    if (splitNode != nullptr) {
        void* nextNode = *reinterpret_cast<void**>(splitNode);
        *reinterpret_cast<void**>(splitNode) = nullptr;

        freeList_[index] = start;

        freeListSize_[index] = keepNum;

        if (returnNum > 0 && nextNode != nullptr) {
            CentralCache::getInstance().returnRange(nextNode, returnNum* alignedSize, index);
        }
    }
}
size_t ThreadCache::getBatchNum(size_t size) {
    size_t alignedSize = SizeClass::roundUp(size);
    size_t num = MAX_BYTES / alignedSize;

    if (num < 2) num = 2;
    if (num > 64) num = 64;

    return num;
}

bool ThreadCache::shouldReturnToCentralcache(size_t index) {
    size_t size = (index + 1) * ALIGNMENT;
    return freeListSize_[index] >= getBatchNum(size);
}
}