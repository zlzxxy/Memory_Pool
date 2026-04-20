void* CentralCache::fetchRange(size_t index) {
    if (index >= FREE_LIST_SIZE) {
        return nullptr;
    }

    while (locks_[index].test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    
    void* result = nullptr;
    try {
        result = centralFreeList_[index].load(std::memory_order_relaxed);
        if (!result) {
            size_t size = (index + 1) * ALIGNMENT;
            result = fetchFromPageCache(size);
            if (!result) {
                locks_[index].clear(std::memory_order_release);
                return nullptr;
            }
            char* start = static_cast<char*>(result);

            size_t numPages = (size <= SPAN_PAGES * PageCache::PAGE_SIZE) ? SPAN_PAGES : (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;
            size_t blockNum = (numPages * PageCache::PAGE_SIZE) / size;

            if (blockNum > 1) {
                for (size_t i = 1; i < blockNum; i++) {
                    void* current = start + (i - 1) * size;
                    void* next = start + i * size;
                    *reinterpret_cast<void**>(current) = next;
                }
                *reinterpret_cast<void**>(start + (blockNum - 1) * size) = nullptr;

                void* next = *reinterpret_cast<void**>(result);
                *reinterpret_cast<void**>(result) = nullptr;
                centralFreeList_[index].store(
                    next,
                    std::memory_order_release
                );

                size_t trackerIndex = spanCount_++;
                if (trackerIndex < SpanTrackers_.size()) {
                    SpanTrackers_[trackerIndex].spanAddr.store(start, std::memory_order_release);
                    SpanTrackers_[trackerIndex].numPages.store(numPages, std::memory_order_release);
                    SpanTrackers_[trackerIndex].blockCount.store(blockNum, std::memory_order_release);
                    SpanTrackers_[trackerIndex].freeCount.store(blockNum - 1, std::memory_order_release);
                }
            }
            else {
                void* next = *reinterpret_cast<void**>(result);
                *reinterpret_cast<void**>(result) = nullptr;
                centralFreeList_[index].store(next, std::memory_order_release);

                SpanTracker* tracker = getSpanTracker(result);
                if (tracker) {
                    tracker->freeCount.fetch_sub(1, std::memory_order_release);
                }
            }
        }
    }
    catch (...) {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    locks_[index].clear(std::memory_order_release);
    return result;
}