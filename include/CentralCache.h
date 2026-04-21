//定义从PageCache中拿来的大块span的结构体
struct SpanTracker {
    std::atomic<void*> spanAddr{nullptr};   //span的起始地址
    std::atomic<size_t> numPages{0};    //span占多少页
    std::atomic<size_t> blockCount{0};    //span被切成了多少小块
    std::atomic<size_t> freeCount{0};    //当前有多少个小块处于空闲状态
};

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

    //中心缓存的自由链表，每一个下标index对应一种固定字节的小块
    std::array<std::atomic<void*>, FREE_LIST_SIZE> centralFreeList_;

    //锁，保护对应的中央自由链表
    std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;

    std::array<SpanTracker, 1024> SpanTrackers_;
    //每次从PageCache拿到一个新的span，spanCount_++
    std::atomic<size_t> spanCount_{0};

    //延迟归还span
    static const size_t MAX_DELAY_COUNT = 48;
    std::array<std::atomic<size_t>, FREE_LIST_SIZE> delayCounts_;
    std::array<std::chrono::steady_clock::time_point, FREE_LIST_SIZE> lastReturnTimes_;
    static const std::chrono::milliseconds DELAY_INTERVAL;

    bool shouldPerformDelayedReturn(size_t index, size_t currentCount, std::chrono::steady_clock::time_point currentTime);
    void performDelayedReturn(size_t index);
};