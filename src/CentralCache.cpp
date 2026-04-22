#include "CentralCache.h"
#include "PageCache.h"
#include <cassert>
#include <thread>
#include <chrono>

namespace memoryPool {
const std::chrono::milliseconds CentralCache::DELAY_INTERVAL{100};

CentralCache::CentralCache() {
    auto now = std::chrono::steady_clock::now();

    for (auto& head : centralFreeList_) {
        head.store(nullptr, std::memory_order_relaxed);
    }

    for (auto& lock : locks_) {
        lock.clear();
    }

    for (auto& count : delayCounts_) {
        count.store(0, std::memory_order_relaxed);
    }

    for (auto& t : lastReturnTimes_) {
        t = now;
    }
}

void* CentralCache::fetchRange(size_t index) {
    if (index >= FREE_LIST_SIZE) {
        return nullptr;
    }

    while (locks_[index].test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    try {
        size_t size = (index + 1) * ALIGNMENT;
        void* head = centralFreeList_[index].load(std::memory_order_relaxed);

        if (!head) {
            void* spanStart = fetchFromPageCache(size);
            if (!spanStart) {
                locks_[index].clear(std::memory_order_release);
                return nullptr;
            }

            char* start = static_cast<char*>(spanStart);

            size_t numPages = (size <= SPAN_PAGES * PageCache::PAGE_SIZE)
                ? SPAN_PAGES
                : (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;
            size_t blockNum = (numPages * PageCache::PAGE_SIZE) / size;

            for (size_t i = 0; i + 1 < blockNum; ++i) {
                void* current = start + i * size;
                void* next = start + (i + 1) * size;
                *reinterpret_cast<void**>(current) = next;
            }
            *reinterpret_cast<void**>(start + (blockNum - 1) * size) = nullptr;

            centralFreeList_[index].store(start, std::memory_order_release);
            head = start;

            size_t trackerIndex = spanCount_.fetch_add(1, std::memory_order_relaxed);
            if (trackerIndex < SpanTrackers_.size()) {
                SpanTrackers_[trackerIndex].spanAddr.store(start, std::memory_order_release);
                SpanTrackers_[trackerIndex].numPages.store(numPages, std::memory_order_release);
                SpanTrackers_[trackerIndex].blockCount.store(blockNum, std::memory_order_release);
                SpanTrackers_[trackerIndex].freeCount.store(blockNum, std::memory_order_release);
                SpanTrackers_[trackerIndex].active.store(true, std::memory_order_release);
            }
        }

        size_t batchNum = std::max<size_t>(2, std::min<size_t>(64, MAX_BYTES / size));

        void* batchHead = head;
        void* prev = nullptr;
        size_t actualNum = 0;

        while (head && actualNum < batchNum) {
            prev = head;
            head = *reinterpret_cast<void**>(head);
            ++actualNum;
        }

        if (prev) {
            *reinterpret_cast<void**>(prev) = nullptr;
        }

        centralFreeList_[index].store(head, std::memory_order_release);

        SpanTracker* tracker = getSpanTracker(batchHead);
        if (tracker) {
            tracker->freeCount.fetch_sub(actualNum, std::memory_order_release);
        }

        locks_[index].clear(std::memory_order_release);
        return batchHead;
    }
    catch (...) {
        locks_[index].clear(std::memory_order_release);
        throw;
    }
}

void CentralCache::returnRange(void* start, size_t size, size_t index) {
    if (!start || index >= FREE_LIST_SIZE) return;
    size_t blockSize = (index + 1) * ALIGNMENT;
    size_t blockCount = size / blockSize;

    while (locks_[index].test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    try {
        void* end = start;
        size_t count = 1;
        while (*reinterpret_cast<void**>(end) != nullptr && count < blockCount) {
            end = *reinterpret_cast<void**>(end);
            count++;
        }

        void* current = centralFreeList_[index].load(std::memory_order_relaxed);
        *reinterpret_cast<void**>(end) = current;
        centralFreeList_[index].store(start, std::memory_order_release);

        size_t currentCount = delayCounts_[index].fetch_add(1, std::memory_order_relaxed) + 1;
        auto currentTime = std::chrono::steady_clock::now();

        if (shouldPerformDelayedReturn(index, currentCount, currentTime)) {
            performDelayedReturn(index);
        }
    }
    catch (...) {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    locks_[index].clear(std::memory_order_release);
}

bool CentralCache::shouldPerformDelayedReturn(size_t index, size_t currentCount, std::chrono::steady_clock::time_point currentTime) {
    if (currentCount >= MAX_DELAY_COUNT) return true;
    auto lastTime = lastReturnTimes_[index];
    return (currentTime - lastTime) >= DELAY_INTERVAL;
}

void CentralCache::performDelayedReturn(size_t index) {
    delayCounts_[index].store(0, std::memory_order_relaxed);
    lastReturnTimes_[index] = std::chrono::steady_clock::now();

    std::unordered_map<SpanTracker*, size_t> spanFreeCounts;
    void* currentBlock = centralFreeList_[index].load(std::memory_order_relaxed);

    while (currentBlock) {
        SpanTracker* tracker = getSpanTracker(currentBlock);
        if (tracker) {
            spanFreeCounts[tracker]++;
        }
        currentBlock = *reinterpret_cast<void**>(currentBlock);
    }

    for (const auto& [tracker, currentCentralFreeBlocks] : spanFreeCounts) {
        updateSpanFreeCount(tracker, currentCentralFreeBlocks, index);
    }
}

void CentralCache::updateSpanFreeCount(SpanTracker* tracker, size_t currentCentralFreeBlocks, size_t index) {
    tracker->freeCount.store(currentCentralFreeBlocks, std::memory_order_release);

    if (currentCentralFreeBlocks == tracker->blockCount.load(std::memory_order_relaxed)) {
        void* spanAddr = tracker->spanAddr.load(std::memory_order_relaxed);
        size_t numPages = tracker->numPages.load(std::memory_order_relaxed);

        void* head = centralFreeList_[index].load(std::memory_order_relaxed);
        void* newHead = head;
        void* prev = nullptr;
        void* current = head;

        while (current) {
            void* next = *reinterpret_cast<void**>(current);
            if (current >= spanAddr && current < static_cast<char*>(spanAddr) + numPages * PageCache::PAGE_SIZE) {
                if (prev) {
                    *reinterpret_cast<void**>(prev) = next;
                }
                else {
                    newHead = next;
                }
            }
            else {
                prev = current;
            }
            current = next;
        }

        centralFreeList_[index].store(newHead, std::memory_order_release);

        tracker->active.store(false, std::memory_order_release);
        tracker->spanAddr.store(nullptr, std::memory_order_release);
        tracker->numPages.store(0, std::memory_order_release);
        tracker->blockCount.store(0, std::memory_order_release);
        tracker->freeCount.store(0, std::memory_order_release);

        PageCache::getInstance().deallocateSpan(spanAddr, numPages);
    }
}

void* CentralCache::fetchFromPageCache(size_t size) {
    size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    if (size <= SPAN_PAGES * PageCache::PAGE_SIZE) {
        return PageCache::getInstance().allocateSpan(SPAN_PAGES);
    }
    else {
        return PageCache::getInstance().allocateSpan(numPages);
    }
}

SpanTracker* CentralCache::getSpanTracker(void* blockAddr) {
    size_t trackerCount = std::min(spanCount_.load(std::memory_order_relaxed), SpanTrackers_.size());

    for (size_t i = 0; i < trackerCount; i++) {
        if (!SpanTrackers_[i].active.load(std::memory_order_relaxed)) {
            continue;
        }

        void* spanAddr = SpanTrackers_[i].spanAddr.load(std::memory_order_relaxed);
        size_t numPages = SpanTrackers_[i].numPages.load(std::memory_order_relaxed);

        if (spanAddr &&
            blockAddr >= spanAddr &&
            blockAddr < static_cast<char*>(spanAddr) + numPages * PageCache::PAGE_SIZE) {
            return &SpanTrackers_[i];
        }
    }

    return nullptr;
}
}