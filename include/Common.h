#pragma once
#include <cstddef>

namespace memoryPool
{
constexpr size_t SPAN_PAGES = 8;    // 小对象默认一次从PageCache拿8页
constexpr size_t ALIGNMENT = 8;    // 所有小块按8字节对齐
constexpr size_t MAX_BYTES = 256 * 1024;    // 内存池最多管理256KB以内的块
constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT;    // 自由链表数量

// 把用户申请的任意大小，映射成内存池中的规格
class SizeClass
{
public:
    static inline size_t roundUp(size_t bytes)
    {
        return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    }

    static inline size_t getIndex(size_t bytes)
    {
        return (bytes <= ALIGNMENT) ? 0 : ((bytes - 1) >> 3);
    }

    static inline size_t indexToSize(size_t index)
    {
        return (index + 1) << 3;
    }
};

}