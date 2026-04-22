#include "CentralCache.h"
#include "PageCache.h"

#include <algorithm>

namespace memoryPool {

namespace {
inline size_t getBatchNumBySize(size_t size) {
    constexpr size_t TARGET_BYTES = 64 * 1024;
    constexpr size_t MIN_BATCH = 8;
    constexpr size_t MAX_BATCH = 512;

    size_t batch = TARGET_BYTES / std::max<size_t>(size, ALIGNMENT);
    if (batch < MIN_BATCH) batch = MIN_BATCH;
    if (batch > MAX_BATCH) batch = MAX_BATCH;
    return batch;
}
}

void* CentralCache::fetchRange(size_t index, size_t& actualNum) {
    actualNum = 0;
    if (index >= FREE_LIST_SIZE) {
        return nullptr;
    }

    const size_t size = (index + 1) * ALIGNMENT;
    const size_t batchNum = getBatchNumBySize(size);

    std::lock_guard<std::mutex> lock(locks_[index]);

    void* head = centralFreeList_[index];
    if (!head) {
        void* spanStart = fetchFromPageCache(size);
        if (!spanStart) {
            return nullptr;
        }

        char* start = static_cast<char*>(spanStart);
        const size_t numPages = (size <= SPAN_PAGES * PageCache::PAGE_SIZE)
            ? SPAN_PAGES
            : (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;
        const size_t blockNum = (numPages * PageCache::PAGE_SIZE) / size;

        for (size_t i = 0; i + 1 < blockNum; ++i) {
            void* current = start + i * size;
            void* next = start + (i + 1) * size;
            *reinterpret_cast<void**>(current) = next;
        }
        *reinterpret_cast<void**>(start + (blockNum - 1) * size) = nullptr;

        head = start;
        centralFreeList_[index] = head;
    }

    void* batchHead = head;
    void* prev = nullptr;

    while (head && actualNum < batchNum) {
        prev = head;
        head = *reinterpret_cast<void**>(head);
        ++actualNum;
    }

    if (prev) {
        *reinterpret_cast<void**>(prev) = nullptr;
    }

    centralFreeList_[index] = head;
    return batchHead;
}

void CentralCache::returnRange(void* start, size_t blockCount, size_t index) {
    if (!start || index >= FREE_LIST_SIZE || blockCount == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(locks_[index]);

    void* end = start;
    for (size_t i = 1; i < blockCount; ++i) {
        end = *reinterpret_cast<void**>(end);
        if (!end) {
            break;
        }
    }

    *reinterpret_cast<void**>(end) = centralFreeList_[index];
    centralFreeList_[index] = start;
}

void* CentralCache::fetchFromPageCache(size_t size) {
    const size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    if (size <= SPAN_PAGES * PageCache::PAGE_SIZE) {
        return PageCache::getInstance().allocateSpan(SPAN_PAGES);
    }

    return PageCache::getInstance().allocateSpan(numPages);
}

}