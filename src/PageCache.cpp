//内存分配
void* PageCache::allocateSpan(size_t numPages) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = freeSpans_.lower_bound(numPages);
    if (it != freeSpans_.end()) {
        Span* span = it->second;
        
        if (span->next) {
            freeSpans_[it->first] = span->next;
        }
        else {
            freeSpans_.erase(it);
        }

        //如果span大于需要的numPages则进行分割
        if (span->numPages > numPages) {
            Span* newSpan = new Span;
            newSpan->pageAddr = static_cast<char*>(span->pageAddr) + numPages * PAGE_SIZE;
            newSpan->numPages = span->numPages - numPages;
            newSpan->next = nullptr;

            auto& list = freeSpans_[newSpan->numPages];
            newSpan->next = list;
            list = newSpan;

            span->numPages = numPages;
        }

        spanMap_[span->pageAddr] = span;
        return span->pageAddr;
    }
}