//ThreadCache从CentralCache中取一块内存
void* CentralCache::fetchRange(size_t index) {
    if (index >= FREE_LIST_SIZE) {
        return nullptr;
    }

    //加锁
    while (locks_[index].test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    
    void* result = nullptr;
    try {
        result = centralFreeList_[index].load(std::memory_order_relaxed);    //取链表头结点
        if (!result) {
            size_t size = (index + 1) * ALIGNMENT;
            result = fetchFromPageCache(size);    //去PageCache申请一整段span
            if (!result) {
                locks_[index].clear(std::memory_order_release);
                return nullptr;
            }
            char* start = static_cast<char*>(result);
            
            //计算span大块能切成多少小块
            size_t numPages = (size <= SPAN_PAGES * PageCache::PAGE_SIZE) ? SPAN_PAGES : (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;
            size_t blockNum = (numPages * PageCache::PAGE_SIZE) / size;

            if (blockNum >= 1) {
                //把一个span切成单链表
                for (size_t i = 1; i < blockNum; i++) {
                    void* current = start + (i - 1) * size;
                    void* next = start + i * size;
                    *reinterpret_cast<void**>(current) = next;
                }
                *reinterpret_cast<void**>(start + (blockNum - 1) * size) = nullptr;

                //使用第一块，其余块挂入中心缓存的自由链表
                void* next = *reinterpret_cast<void**>(result);
                *reinterpret_cast<void**>(result) = nullptr;
                centralFreeList_[index].store(
                    next,
                    std::memory_order_release
                );

                //登记span的元信息
                size_t trackerIndex = spanCount_++;
                if (trackerIndex < SpanTrackers_.size()) {
                    SpanTrackers_[trackerIndex].spanAddr.store(start, std::memory_order_release);
                    SpanTrackers_[trackerIndex].numPages.store(numPages, std::memory_order_release);
                    SpanTrackers_[trackerIndex].blockCount.store(blockNum, std::memory_order_release);
                    SpanTrackers_[trackerIndex].freeCount.store(blockNum - 1, std::memory_order_release);
                }
            }
        }
        else {
            void* next = *reinterpret_cast<void**>(result);
            *reinterpret_cast<void**>(result) = nullptr;
            centralFreeList_[index].store(next, std::memory_order_release);

            //这个块从“空闲”变成“已分配”，它所属span的空闲块数量要减1
            SpanTracker* tracker = getSpanTracker(result);
            if (tracker) {
                tracker->freeCount.fetch_sub(1, std::memory_order_release);
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