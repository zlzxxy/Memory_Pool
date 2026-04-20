struct SpanTracker {
    std::atomic<void*> spacAddr{nullptr};
    std::atomic<size_t> numPages{0};
    std::atomic<size_t> blockCount{0};
    std::atomic<size_t> freeCount{0};
}

class CentralCache {
public:
    static CentralCache& getInstance() {
        static CentralCache instance;
        return instance;
    }

    void* fetchRange(size_t index);
    void returnRange(void* start, size_t size, size_t index);

private:
    CentralCache();
    void* fetchFromPageCache(size_t size);

    SpanTracker* getSpanTracker(void* blockAddr);

    void updateSpanFreeCount(SpanTracker* tracker, size_t newFreeBlocks, size_t index);

private:
    //中心缓存的自由链表
    std::array<std::atomic<void*>, FREE_LIST_SIZE> centralFreeList_;

    std::array<std::atomic_falg, FREE_LIST_SIZE> locks_;

    std::array<SpanTracker, 1024> spacTrackers_;
    std::atomic<size_t> spanCount_{0};

    static const size_t MAX_DELAY_COUNT = 48;
    std::array<std::atomic<size_t>, FREE_LIST_SIZE> delayCounts_;
    std::array<std::chrono::steady_clock::time_point, FREE_LIST_SIZE> lastReturnTimes_;
    static const std::chrono::milliseconds DELAY_INTERVAL;

    bool shouldPerformDelayedReturn(size_t index, size_t currentCount, std::chrono::steady_clock::time_point currentTime);
    void performDelayedReturn(size_t index);
}