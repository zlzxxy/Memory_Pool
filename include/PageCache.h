// 最底层的缓存
#pragma once
#include "Common.h"
#include <map>
#include <mutex>

namespace memoryPool {
class PageCache {
public:
    static const size_t PAGE_SIZE = 4096;   // 一页4KB

    static PageCache& getInstance() {
        static PageCache instance;
        return instance;
    }

    void* allocateSpan(size_t numPages);

    void deallocateSpan(void* ptr, size_t numPages);

private:
    // 一个Span表示一段连续的页
    struct Span {
        void* pageAddr;
        size_t numPages;
        Span* next;
        bool isFree;
    };

    PageCache() = default;

    void* systemAlloc(size_t numPages);

    bool removeFromFreeList(Span* span);

private:
    // value为Span*是因为同样页数的Span可能有多个，所以用链表串起来
    std::map<size_t, Span*> freeSpans_;    // 按页数管理空闲Span
    std::map<void*, Span*> spanMap_;    // 按照起始地址查找Span
    std::mutex mutex_;
};

}