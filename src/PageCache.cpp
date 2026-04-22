#include "PageCache.h"
#include <sys/mman.h>
#include <cstring>

namespace memoryPool {

bool PageCache::removeFromFreeList(Span* span) {
    auto it = freeSpans_.find(span->numPages);
    if (it == freeSpans_.end()) {
        return false;
    }

    Span* current = it->second;
    Span* prev = nullptr;
    while (current && current != span) {
        prev = current;
        current = current->next;
    }

    if (!current) {
        return false;
    }

    if (prev) {
        prev->next = current->next;
    } else {
        it->second = current->next;
    }

    if (it->second == nullptr) {
        freeSpans_.erase(it);
    }

    current->next = nullptr;
    current->isFree = false;
    return true;
}

void* PageCache::allocateSpan(size_t numPages) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = freeSpans_.lower_bound(numPages);
    if (it != freeSpans_.end()) {
        Span* span = it->second;

        if (span->next) {
            it->second = span->next;
        }
        else {
            freeSpans_.erase(it);
        }

        span->next = nullptr;
        span->isFree = false;

        if (span->numPages > numPages) {
            Span* newSpan = new Span;
            newSpan->pageAddr = static_cast<char*>(span->pageAddr) + numPages * PAGE_SIZE;
            newSpan->numPages = span->numPages - numPages;
            newSpan->next = nullptr;
            newSpan->isFree = true;

            auto& list = freeSpans_[newSpan->numPages];
            newSpan->next = list;
            list = newSpan;
            spanMap_[newSpan->pageAddr] = newSpan;

            span->numPages = numPages;
        }

        spanMap_[span->pageAddr] = span;
        return span->pageAddr;
    }
    else {
        void* memory = systemAlloc(numPages);
        if (!memory) return nullptr;

        Span* span = new Span;
        span->pageAddr = memory;
        span->numPages = numPages;
        span->next = nullptr;
        span->isFree = false;

        spanMap_[memory] = span;
        return memory;
    }
}

void PageCache::deallocateSpan(void* ptr, size_t /*numPages*/) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = spanMap_.find(ptr);
    if (it == spanMap_.end()) return;

    Span* span = it->second;
    span->isFree = true;

    void* nextAddr = static_cast<char*>(span->pageAddr) + span->numPages * PAGE_SIZE;
    auto nextIt = spanMap_.find(nextAddr);
    if (nextIt != spanMap_.end()) {
        Span* nextSpan = nextIt->second;
        if (nextSpan != span && nextSpan->isFree && removeFromFreeList(nextSpan)) {
            span->numPages += nextSpan->numPages;
            spanMap_.erase(nextAddr);
            delete nextSpan;
        }
    }

    auto& list = freeSpans_[span->numPages];
    span->next = list;
    list = span;
}

void* PageCache::systemAlloc(size_t numPages)
{
    size_t size = numPages * PAGE_SIZE;

    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) return nullptr;

    return ptr;
}
}