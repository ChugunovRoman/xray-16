#pragma once

#include "xrCore/Profiling/PerfMetrics.hpp"

namespace xray::render::RENDER_NAMESPACE
{
IC void dx11ConstantBuffer::Write(u32 offset, const void* src, size_t size)
{
    //	Check buffer size in client code: don't know if actual data will cross
    //	buffer boundaries.
    VERIFY(offset + size <= m_uiBufferSize);
    u8* dst = static_cast<u8*>(m_pBufferData) + offset;
    // Most per-draw sets re-send equal values (hemi/material/xform of the same object, bones shared by the
    // children of one skeleton). Marking the buffer changed on every set turned each of them into a full
    // Map(WRITE_DISCARD) + copy + Unmap in Flush() - ~2.3 per dynamic draw in the main view.
    // Equal bytes may be skipped only if the GPU copy holds them: after a partial flush (see WriteRows)
    // the palette rows past the uploaded prefix are undefined on the GPU.
    const u32 end = offset + u32(size);
    if (ValidOnGpu(offset, end) && memcmp(dst, src, size) == 0)
    {
        PERF_INC(cb_write_same);
        return;
    }
    CopyMemory(dst, src, size);
    if (end <= m_uiArrayOffset || offset >= m_uiArrayEnd)
    {
        // A member outside the palette array (m_invW of parallax shaders sits after it in $Globals): always copied,
        // so a partial Flush stays possible - it only has to keep the palette rows the GPU copy holds now.
        if (!m_bChanged)
        {
            m_bChanged = true;
            m_uiFlushEnd = std::max(m_uiArrayOffset, m_uiUploadEnd);
        }
        return;
    }
    m_bChanged = true;
    m_uiFlushEnd = m_uiBufferSize; // an ordinary set: the whole buffer goes up
}

IC void dx11ConstantBuffer::WriteRows(u32 offset, const void* src, u32 size)
{
    const u32 end = offset + size;
    VERIFY(end <= m_uiBufferSize);
    u8* dst = static_cast<u8*>(m_pBufferData) + offset;
    if (ValidOnGpu(offset, end) && memcmp(dst, src, size) == 0)
    {
        PERF_INC(cb_write_same);
        return;
    }
    CopyMemory(dst, src, size);

    // A partial upload only for rows inside the palette array: the draw that follows reads no element of it past
    // `end` (CSkeletonX writes the whole [0, RMS_bonecount * 3) range its vertices can index), every other member is
    // always copied. Anything else - the whole buffer, as before.
    if (offset < m_uiArrayOffset || end > m_uiArrayEnd)
    {
        m_bChanged = true;
        m_uiFlushEnd = m_uiBufferSize;
        return;
    }
    if (m_bChanged)
        m_uiFlushEnd = std::max(m_uiFlushEnd, end); // a whole-buffer flush stays whole, a partial one grows
    else
    {
        m_bChanged = true;
        m_uiFlushEnd = end;
    }
}

IC void dx11ConstantBuffer::set(R_constant* C, R_constant_load& L, const Fmatrix& A)
{
    VERIFY(RC_float == C->type);
    Fvector4 rows[4];
    rows[0].set(A._11, A._21, A._31, A._41);
    rows[1].set(A._12, A._22, A._32, A._42);
    rows[2].set(A._13, A._23, A._33, A._43);
    rows[3].set(A._14, A._24, A._34, A._44);
    switch (L.cls)
    {
    case RC_2x4: Write(L.index, rows, 2 * lineSize); break;
    case RC_3x4: Write(L.index, rows, 3 * lineSize); break;
    case RC_4x4: Write(L.index, rows, 4 * lineSize); break;
    default:
#ifdef DEBUG
        xrDebug::Fatal(DEBUG_INFO, "Invalid constant run-time-type for '%s'", C->name.c_str());
#else
        NODEFAULT;
#endif
    }
}

IC void dx11ConstantBuffer::set(R_constant* C, R_constant_load& L, const Fvector4& A)
{
    VERIFY(RC_float == C->type);
    VERIFY(RC_1x4 == L.cls || RC_1x3 == L.cls || RC_1x2 == L.cls);
    VERIFY(u32((u32)L.index + lineSize) <= m_uiBufferSize);

    size_t count = 4;
    switch (L.cls)
    {
    case RC_1x2: count = 2; break;
    case RC_1x3: count = 3; break;
    case RC_1x4: count = 4; break;
    default: break;
    }

    Write(L.index, &A[0], count * sizeof(float));
}

IC void dx11ConstantBuffer::set(R_constant* C, R_constant_load& L, float A)
{
    VERIFY(RC_float == C->type);
    VERIFY(RC_1x1 == L.cls);
    Write(L.index, &A, sizeof(float));
}

IC void dx11ConstantBuffer::set(R_constant* C, R_constant_load& L, int A)
{
    VERIFY(RC_int == C->type);
    VERIFY(RC_1x1 == L.cls);
    Write(L.index, &A, sizeof(int));
}

IC void dx11ConstantBuffer::seta(R_constant* C, R_constant_load& L, u32 e, const Fmatrix& A)
{
    VERIFY(RC_float == C->type);
    Fvector4 rows[4];
    rows[0].set(A._11, A._21, A._31, A._41);
    rows[1].set(A._12, A._22, A._32, A._42);
    rows[2].set(A._13, A._23, A._33, A._43);
    rows[3].set(A._14, A._24, A._34, A._44);
    switch (L.cls)
    {
    case RC_2x4: Write((u32)L.index + 2 * lineSize * e, rows, 2 * lineSize); break;
    case RC_3x4: Write((u32)L.index + 3 * lineSize * e, rows, 3 * lineSize); break;
    case RC_4x4: Write((u32)L.index + 4 * lineSize * e, rows, 4 * lineSize); break;
    default:
#ifdef DEBUG
        xrDebug::Fatal(DEBUG_INFO, "Invalid constant run-time-type for '%s'", C->name.c_str());
#else
        NODEFAULT;
#endif
    }
}

IC void dx11ConstantBuffer::seta(R_constant* C, R_constant_load& L, u32 e, const Fvector4& A)
{
    VERIFY(RC_float == C->type);
    VERIFY(RC_1x4 == L.cls || RC_1x3 == L.cls || RC_1x2 == L.cls);

    // Arrays of vectors always occupy whole 16-byte registers (bones: 3 rows per bone via set_ca).
    Write((u32)L.index + lineSize * e, &A, lineSize);
}

IC void dx11ConstantBuffer::seta_rows(R_constant* C, R_constant_load& L, u32 e, const Fvector4* rows, u32 count)
{
    VERIFY(RC_float == C->type);
    VERIFY(RC_1x4 == L.cls || RC_1x3 == L.cls || RC_1x2 == L.cls);
    // Same layout as `count` seta(Fvector4) calls: one 16-byte register per element, back to back.
    WriteRows((u32)L.index + lineSize * e, rows, lineSize * count);
}

IC void* dx11ConstantBuffer::AccessDirect(R_constant_load& L, size_t DataSize)
{
    //	Check buffer size in client code: don't know if actual data will cross
    //	buffer boundaries.
    VERIFY(L.index < (int)m_uiBufferSize);
    u8* res = ((u8*)m_pBufferData) + L.index;

    if ((size_t)L.index + DataSize <= m_uiBufferSize)
    {
        // The caller writes through the pointer itself - no way to tell whether anything changed.
        m_bChanged = true;
        m_uiFlushEnd = m_uiBufferSize;
        return res;
    }
    else
        return 0;
}
} // namespace xray::render::RENDER_NAMESPACE
