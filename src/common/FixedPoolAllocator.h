/*
 * Surge XT - a free and open source hybrid synthesizer,
 * built by Surge Synth Team
 *
 * Learn more at https://surge-synthesizer.github.io/
 *
 * Copyright 2018-2024, various authors, as described in the GitHub
 * transaction log.
 *
 * Surge XT is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later). The license is found in the "LICENSE"
 * file in the root of this repository, or at
 * https://www.gnu.org/licenses/gpl-3.0.en.html
 *
 * Surge was a commercial product from 2004-2018, copyright and ownership
 * held by Claes Johanson at Vember Audio during that period.
 * Claes made Surge open source in September 2018.
 *
 * All source for Surge XT is available at
 * https://github.com/surge-synthesizer/surge
 */

#ifndef SURGE_SRC_COMMON_FIXEDPOOLALLOCATOR_H
#define SURGE_SRC_COMMON_FIXEDPOOLALLOCATOR_H

#include <cstddef>
#include <memory>
#include <new>
#include <vector>

namespace Surge
{
/*
 * An allocator for node containers (std::list) used on the audio thread. The container's
 * node allocator gets a pool of N nodes when the container is constructed, so inserting and
 * erasing don't touch the heap. Past N nodes it falls back to operator new.
 *
 * Copies share the pool; allocators for different types (the container's value type versus
 * its node type) each get their own pool when rebound.
 */
template <typename T, size_t N> struct FixedPoolAllocator
{
    using value_type = T;
    template <typename U> struct rebind
    {
        using other = FixedPoolAllocator<U, N>;
    };

    struct Pool
    {
        std::vector<T *> freeList;
        std::unique_ptr<std::aligned_storage_t<sizeof(T), alignof(T)>[]> storage;
        Pool() : storage(new std::aligned_storage_t<sizeof(T), alignof(T)>[N])
        {
            freeList.reserve(N);
            for (size_t i = 0; i < N; ++i)
                freeList.push_back(reinterpret_cast<T *>(&storage[N - 1 - i]));
        }
        bool owns(const T *p) const
        {
            auto b = reinterpret_cast<const T *>(&storage[0]);
            return p >= b && p < b + N;
        }
    };
    std::shared_ptr<Pool> pool;

    FixedPoolAllocator() : pool(std::make_shared<Pool>()) {}
    template <typename U>
    FixedPoolAllocator(const FixedPoolAllocator<U, N> &) : pool(std::make_shared<Pool>())
    {
    }

    T *allocate(size_t n)
    {
        if (n == 1 && !pool->freeList.empty())
        {
            auto p = pool->freeList.back();
            pool->freeList.pop_back();
            return p;
        }
        return static_cast<T *>(::operator new(n * sizeof(T)));
    }
    void deallocate(T *p, size_t n)
    {
        if (n == 1 && pool->owns(p))
            pool->freeList.push_back(p);
        else
            ::operator delete(p);
    }

    template <typename U> bool operator==(const FixedPoolAllocator<U, N> &o) const
    {
        return (const void *)pool.get() == (const void *)o.pool.get();
    }
    template <typename U> bool operator!=(const FixedPoolAllocator<U, N> &o) const
    {
        return !(*this == o);
    }
};
} // namespace Surge

#endif // SURGE_SRC_COMMON_FIXEDPOOLALLOCATOR_H
