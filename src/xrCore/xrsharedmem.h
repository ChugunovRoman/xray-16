#pragma once
#ifndef xrsharedmemH
#define xrsharedmemH

//#include "_stl_extensions.h"
#include "xrCommon/xr_vector.h"
#include "xrCommon/xr_unordered_map.h"
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif
#include "Common/Noncopyable.hpp"

// fwd. decl.
class Lock;

#pragma pack(push, 4)
//////////////////////////////////////////////////////////////////////////
#pragma warning(push)
#pragma warning(disable : 4200)
struct XRCORE_API smem_value
{
    u32 dwReference;
    u32 dwCRC;
    u32 dwLength; // XXX: make size_t
    u32 _align_16;
    u8 value[];
};
// generic predicate for "less"
IC bool smem_sort(const smem_value* A, const smem_value* B)
{
    if (A->dwCRC < B->dwCRC)
        return true;
    if (A->dwCRC > B->dwCRC)
        return false;
    if (A->dwLength < B->dwLength)
        return true;
    if (A->dwLength > B->dwLength)
        return false;
    return memcmp(A->value, B->value, A->dwLength) < 0;
};

// predicate for insertion - just a quick estimate
IC bool smem_search(const smem_value* A, const smem_value* B)
{
    if (A->dwCRC < B->dwCRC)
        return true;
    if (A->dwCRC > B->dwCRC)
        return false;
    return A->dwLength < B->dwLength;
};

// predicate for exact (byte level) comparition
IC bool smem_equal(const smem_value* A, u32 dwCRC, u32 dwLength, u8* ptr)
{
    if (A->dwCRC != dwCRC)
        return false;
    if (A->dwLength != dwLength)
        return false;
    return 0 == memcmp(A->value, ptr, dwLength);
};
#pragma warning(pop)

//////////////////////////////////////////////////////////////////////////
class XRCORE_API smem_container : Noncopyable
{

public:
    smem_container();
    ~smem_container();

    smem_value* dock(u32 dwCRC, u32 dwLength, void* ptr);
    void clean();
    void dump();

    [[nodiscard]]
    size_t stat_economy() const;

private:
    // 40-parallel-prefetch.md (plans/optimization_mt): a hash on (crc, length) instead of a vector sorted by them. A
    // dock of a new value inserted into the middle of the vector - a move of half of it, and every key track of an omf
    // is docked (hundreds of thousands of values by the end of the model prefetch). Values of equal crc and length are
    // compared byte by byte, as before.
    using cdb = std::unordered_multimap<u64, smem_value*, std::hash<u64>, std::equal_to<u64>,
        xr_allocator<std::pair<const u64, smem_value*>>>;
    mutable Lock lock;
    cdb container;
};
XRCORE_API extern smem_container* g_pSharedMemoryContainer;

// 40-parallel-prefetch.md: the reference count of smem_value is changed interlocked (as the one of str_value,
// xrstring.h): skinned models load on several threads at once (CSkeletonX::_Load docks BonesUsed) while the main
// thread copies loaded ones (CSkeletonX::_Copy); a lost increment frees a live value at the next clean(). The field
// stays a plain u32 (layout; the readers in xrsharedmem.cpp run under the container lock).
namespace xr_smem_detail
{
inline void ref_inc(u32& ref) noexcept
{
#if defined(_MSC_VER) && !defined(__clang__)
    _InterlockedIncrement(reinterpret_cast<volatile long*>(&ref));
#else
    __atomic_add_fetch(&ref, 1u, __ATOMIC_RELAXED);
#endif
}

[[nodiscard]] inline u32 ref_dec(u32& ref) noexcept
{
#if defined(_MSC_VER) && !defined(__clang__)
    return static_cast<u32>(_InterlockedDecrement(reinterpret_cast<volatile long*>(&ref)));
#else
    return __atomic_sub_fetch(&ref, 1u, __ATOMIC_ACQ_REL);
#endif
}
} // namespace xr_smem_detail

//////////////////////////////////////////////////////////////////////////
template <class T>
class ref_smem
{
private:
    smem_value* p_;

protected:
    // ref-counting
    void _dec()
    {
        if (0 == p_)
            return;
        if (0 == xr_smem_detail::ref_dec(p_->dwReference))
            p_ = 0;
    }

public:
    void _set(ref_smem const& rhs)
    {
        smem_value* v = rhs.p_;
        if (0 != v)
            xr_smem_detail::ref_inc(v->dwReference);
        _dec();
        p_ = v;
    }

    [[nodiscard]]
    const smem_value* _get() const { return p_; }
public:
    // construction
    ref_smem() { p_ = 0; }
    ref_smem(ref_smem<T> const& rhs)
    {
        p_ = 0;
        _set(rhs);
    }
    ~ref_smem() { _dec(); }
    void create(u32 dwCRC, u32 dwLength, T* ptr)
    {
        smem_value* v = g_pSharedMemoryContainer->dock(dwCRC, dwLength * sizeof(T), ptr);
        if (0 != v)
            xr_smem_detail::ref_inc(v->dwReference);
        _dec();
        p_ = v;
    }

    // assignment & accessors
    ref_smem<T>& operator=(ref_smem<T> const& rhs)
    {
        _set(rhs);
        return (ref_smem<T>&)*this;
    }
    T* operator*() const { return p_ ? (T*)p_->value : 0; }
    bool operator!() const { return p_ == 0; }
    T& operator[](size_t id) { return ((T*)(p_->value))[id]; }
    const T& operator[](size_t id) const { return ((T*)(p_->value))[id]; }

    // misc func
    [[nodiscard]]
    u32 size() const
    {
        if (0 == p_)
            return 0;

        return p_->dwLength / sizeof(T);
    }

    void swap(ref_smem<T>& rhs)
    {
        smem_value* tmp = p_;
        p_ = rhs.p_;
        rhs.p_ = tmp;
    }
    bool equal(ref_smem<T>& rhs) { return p_ == rhs.p_; }

    [[nodiscard]]
    u32 ref_count() const
    {
        if (0 == p_)
            return 0;

        return p_->dwReference;
    }
};

// res_ptr == res_ptr
// res_ptr != res_ptr
// const res_ptr == ptr
// const res_ptr != ptr
// ptr == const res_ptr
// ptr != const res_ptr
// res_ptr < res_ptr
// res_ptr > res_ptr
// XXX: Make these comparison operators complete (if they are even required). Missing <= and >=.
template <class T>
IC bool operator==(ref_smem<T> const& a, ref_smem<T> const& b)
{
    return a._get() == b._get();
}
template <class T>
IC bool operator!=(ref_smem<T> const& a, ref_smem<T> const& b)
{
    return !(a==b);
}
template <class T>
IC bool operator<(ref_smem<T> const& a, ref_smem<T> const& b)
{
    return a._get() < b._get();
}
template <class T>
IC bool operator>(ref_smem<T> const& a, ref_smem<T> const& b)
{
    return a._get() > b._get();
}

// externally visible standard functionality
template <class T>
IC void swap(ref_smem<T>& lhs, ref_smem<T>& rhs)
{
    lhs.swap(rhs);
}

#pragma pack(pop)

#endif
