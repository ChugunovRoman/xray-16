#include "stdafx.h"
#pragma hdrstop // huh?
#include "Threading/Lock.hpp"

#if defined(XR_PLATFORM_WINDOWS)
#include "xrCore/Text/Utf8Utils.hpp"
#endif

using namespace std;

XRCORE_API smem_container* g_pSharedMemoryContainer = nullptr;

smem_container::smem_container()
#ifdef CONFIG_PROFILE_LOCKS
    : lock(MUTEX_PROFILE_ID(smem_container))
#endif // CONFIG_PROFILE_LOCKS
{
}

smem_container::~smem_container()
{
    clean();
}

namespace
{
// The key of the hash container: values of equal crc and length share it
u64 smem_key(u32 dwCRC, u32 dwLength) { return (u64(dwCRC) << 32) | u64(dwLength); }
} // namespace

smem_value* smem_container::dock(u32 dwCRC, u32 dwLength, void* ptr)
{
    VERIFY(dwCRC && dwLength && ptr);

    ScopeLock scope(&lock);

    // search: the values of this crc and length, byte by byte
    const u64 key = smem_key(dwCRC, dwLength);
    const auto range = container.equal_range(key);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (0 == memcmp(it->second->value, ptr, dwLength))
            return it->second; // really found
    }

    // not found - create new entry
    smem_value* result = (smem_value*)xr_malloc(sizeof(smem_value) + dwLength);
    result->dwReference = 0;
    result->dwCRC = dwCRC;
    result->dwLength = dwLength;
    CopyMemory(result->value, ptr, dwLength);
    container.emplace(key, result);
    return result;
}

void smem_container::clean()
{
    ScopeLock scope(&lock);
    for (auto it = container.begin(); it != container.end();)
    {
        if (0 == it->second->dwReference)
        {
            xr_free(it->second);
            it = container.erase(it);
        }
        else
            ++it;
    }
    if (container.empty())
        container.clear();
}

void smem_container::dump()
{
    ScopeLock scope(&lock);
#if defined(XR_PLATFORM_WINDOWS)
    FILE* F = _wfopen(L"x:\\$smem_dump$.txt", L"w");
#else
    FILE* F = fopen("x:\\$smem_dump$.txt", "w");
#endif
    for (const auto& [key, v] : container)
        fprintf(F, "%4u : crc[%6x], %u bytes\n", v->dwReference, v->dwCRC, v->dwLength);
    fclose(F);
}

size_t smem_container::stat_economy() const
{
    ptrdiff_t counter = 0;
    {
        ScopeLock scope(&lock);
        counter -= sizeof(*this);
        counter -= sizeof(cdb::allocator_type);
        constexpr ptrdiff_t node_size = 20; // XXX: refactor
        for (const auto& [key, v] : container)
        {
            counter -= 16;
            counter -= node_size;
            counter += ptrdiff_t((ptrdiff_t(v->dwReference) - 1) * ptrdiff_t(v->dwLength));
        }
    }

    return size_t(counter / ptrdiff_t(1024));
}
