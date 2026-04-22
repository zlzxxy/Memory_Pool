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

//ThreadCache从CentralCache中取一块内存
void* CentralCache::fetchRange(size_t index) {
    if (index >= FREE_LIST_SIZE) {
        return nullptr;
    }

    // 对当前 size class 加锁，防止多个线程同时修改 centralFreeList_
    while (locks_[index].test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    try {
        size_t size = (index + 1) * ALIGNMENT;
        void* head = centralFreeList_[index].load(std::memory_order_relaxed);

        // 如果当前 central freelist 为空，就从 PageCache 获取一段 span，
        // 再把这段 span 切成固定大小的小块，串成链表
        if (!head) {
            void* spanStart = fetchFromPageCache(size);
            if (!spanStart) {
                locks_[index].clear(std::memory_order_release);
                return nullptr;
            }

            char* start = static_cast<char*>(spanStart);

            // 计算这次 span 占多少页，以及总共能切出多少个小块
            size_t numPages = (size <= SPAN_PAGES * PageCache::PAGE_SIZE)
                ? SPAN_PAGES
                : (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;
            size_t blockNum = (numPages * PageCache::PAGE_SIZE) / size;

            // 把 span 内存按 size 切成链表：block1 -> block2 -> ... -> nullptr
            for (size_t i = 0; i + 1 < blockNum; ++i) {
                void* current = start + i * size;
                void* next = start + (i + 1) * size;
                *reinterpret_cast<void**>(current) = next;
            }
            *reinterpret_cast<void**>(start + (blockNum - 1) * size) = nullptr;

            // 挂到 central freelist 上
            centralFreeList_[index].store(start, std::memory_order_release);
            head = start;

            // 记录这个 span 的基本信息，后续回收时要用
            size_t trackerIndex = spanCount_++;
            if (trackerIndex < SpanTrackers_.size()) {
                SpanTrackers_[trackerIndex].spanAddr.store(start, std::memory_order_release);
                SpanTrackers_[trackerIndex].numPages.store(numPages, std::memory_order_release);
                SpanTrackers_[trackerIndex].blockCount.store(blockNum, std::memory_order_release);
                SpanTrackers_[trackerIndex].freeCount.store(blockNum, std::memory_order_release);
            }
        }

        // 一次不要只返回一个块，而是批量返回一段链表给 ThreadCache
        // 这样可以减少线程频繁访问 CentralCache 的次数
        size_t batchNum = std::max<size_t>(2, std::min<size_t>(64, MAX_BYTES / size));

        void* batchHead = head;   // 记录这一批链表的头结点
        void* prev = nullptr;     // 记录这一批的最后一个结点
        size_t actualNum = 0;     // 实际取出的块数

        // 从 central freelist 中切下前 actualNum 个块
        while (head && actualNum < batchNum) {
            prev = head;
            head = *reinterpret_cast<void**>(head);
            ++actualNum;
        }

        // 断开这一批链表和剩余链表的连接
        if (prev) {
            *reinterpret_cast<void**>(prev) = nullptr;
        }

        // 剩余部分继续留在 central freelist 中
        centralFreeList_[index].store(head, std::memory_order_release);

        // 更新 span 的空闲块数：
        // 从 CentralCache 拿走了 actualNum 个块，所以 freeCount 要减少
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

//把“一段链表”还给CentralCache
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

//集中统计当前链表里每个span有多少空闲块
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

    for (const auto& [tracker, newFreeBlocks] : spanFreeCounts) {
        updateSpanFreeCount(tracker, newFreeBlocks, index);
    }
}

void CentralCache::updateSpanFreeCount(SpanTracker* tracker, size_t newFreeBlocks, size_t index) {
    //更新空闲块数
    size_t oldFreeCount = tracker->freeCount.load(std::memory_order_relaxed);
    size_t newFreeCount = oldFreeCount + newFreeBlocks;
    tracker->freeCount.store(newFreeCount, std::memory_order_release);

    //如果空闲块数等于总块数，将span整段归还给PageCache
    if (newFreeCount == tracker->blockCount.load(std::memory_order_relaxed)) {
        void* spanAddr = tracker->spanAddr.load(std::memory_order_relaxed);
        size_t numPages = tracker->numPages.load(std::memory_order_relaxed);

        void* head = centralFreeList_[index].load(std::memory_order_relaxed);
        void* newHead = head;
        void* prev = nullptr;
        void* current = head;

        while (current) {
            void* next = *reinterpret_cast<void**> (current);
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
    for (size_t i = 0; i < spanCount_.load(std::memory_order_relaxed); i++) {
        void* spanAddr = SpanTrackers_[i].spanAddr.load(std::memory_order_relaxed);
        size_t numPages = SpanTrackers_[i].numPages.load(std::memory_order_relaxed);

        if (blockAddr >= spanAddr && blockAddr < static_cast<char*>(spanAddr) + numPages * PageCache::PAGE_SIZE) {
            return &SpanTrackers_[i];
        }
    }
    return nullptr;
}
}