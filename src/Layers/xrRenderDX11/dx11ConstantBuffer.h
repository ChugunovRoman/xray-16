#pragma once

namespace xray::render::RENDER_NAMESPACE
{
struct R_constant;
struct R_constant_load;

// Per-thread running totals of dx11ConstantBuffer::Flush maps (perf metrics only): render_graph takes deltas
// of these around the main view's static/dynamic submission to get render/main/*_cb_flush.
inline thread_local u32 g_cb_flush_count = 0;
inline thread_local u64 g_cb_flush_bytes = 0;

class dx11ConstantBuffer : public xr_resource_named
{
public:
    dx11ConstantBuffer(ID3DShaderReflectionConstantBuffer* pTable);
    ~dx11ConstantBuffer();

    // The same cbuffer layout - name, size, members (name, offset, type); buffers with it are shared (P15b). Unlike
    // the former Similar it does not compare D3D_SHADER_TYPE_DESC::Name, a pointer into the shader's reflection
    // object: with it no two shaders ever shared a buffer (not even the vertex and pixel shader of one pass), so
    // cbuffer dynamic_transforms was mapped twice per draw.
    bool SameLayout(const dx11ConstantBuffer& _in) const;
    // Creates the GPU buffer. Called only for a buffer that gets registered: a lookup that finds an equal registered
    // buffer drops its temporary one without touching the device.
    void CreateGpuBuffer();
    ID3DBuffer* GetBuffer() { return m_pBuffer; }
    void Flush(u32 context_id);
    // Per-draw entry of R_constants::flush_cache: the m_bChanged test inline, the out-of-line Flush only when there
    // is something to upload.
    void FlushIfChanged(u32 context_id)
    {
        if (m_bChanged)
            Flush(context_id);
    }

    //	Set copy data into constant buffer
    //	Plain buffer member
    void set(R_constant* C, R_constant_load& L, const Fmatrix& A);
    void set(R_constant* C, R_constant_load& L, const Fvector4& A);
    void set(R_constant* C, R_constant_load& L, float A);
    void set(R_constant* C, R_constant_load& L, int A);
    //	Array buffer member
    void seta(R_constant* C, R_constant_load& L, u32 e, const Fmatrix& A);
    void seta(R_constant* C, R_constant_load& L, u32 e, const Fvector4& A);
    // `count` consecutive float4 elements [e, e + count) of an array member in one write (bone palette of
    // CSkeletonX) instead of `count` seta calls.
    void seta_rows(R_constant* C, R_constant_load& L, u32 e, const Fvector4* rows, u32 count);

    void* AccessDirect(R_constant_load& L, size_t DataSize);

    // Forces the next Flush() to re-upload. Needed whenever the GPU copy can no longer be trusted - a pooled
    // deferred context starts a new command list (dynamic buffer contents are undefined there until mapped
    // with WRITE_DISCARD), see CResourceManager::MarkConstantBuffersChanged / CBackend::ResetDeferredCache.
    void MarkChanged()
    {
        m_bChanged = true;
        m_uiFlushEnd = m_uiBufferSize;
        m_uiUploadEnd = 0;
    }

private:
    // Copies `size` bytes into the CPU shadow at `offset`; marks the buffer for re-upload only when the bytes
    // actually differ, instead of on every set as before.
    void Write(u32 offset, const void* src, size_t size);
    // Same for leading rows of the palette array member. The next Flush may then leave out the
    // rest of the array (the draw reads no element past the rows written for it), see Flush.
    void WriteRows(u32 offset, const void* src, u32 size);
    // Whether [offset, end) will be valid in the GPU copy once a pending Flush (if any) has run - only then may an
    // equal write be skipped. Everything outside the palette array, and its rows below the uploaded prefix, are.
    bool ValidOnGpu(u32 offset, u32 end) const
    {
        return end <= (m_bChanged ? m_uiFlushEnd : m_uiUploadEnd) || offset >= m_uiArrayEnd;
    }

private:
    shared_str m_strBufferName;
    D3D_CBUFFER_TYPE m_eBufferType;

    //	Buffer data description
    xr_vector<D3D_SHADER_TYPE_DESC> m_MembersList;
    xr_vector<shared_str> m_MembersNames;
    // SameLayout: type names (by value - m_MembersList[i].Name points into a released reflection object) and offsets.
    xr_vector<shared_str> m_MembersTypeNames;
    xr_vector<u32> m_MembersOffsets;

    ID3DBuffer* m_pBuffer;
    u32 m_uiBufferSize; //	Cache buffer size for debug validation
    void* m_pBufferData;
    bool m_bChanged;

    // Partial palette upload bookkeeping. [m_uiArrayOffset, m_uiArrayEnd): the largest float4 array member (sbones_array
    // in the $Globals of skinned vertex shaders) - the only bytes a partial Flush may leave out; with no such member
    // both are m_uiBufferSize. A partial Flush copies [0, m_uiFlushEnd) and [m_uiArrayEnd, size): every other member
    // plus the leading rows of the array the draw uses. m_uiFlushEnd: m_uiBufferSize for a whole-buffer Flush, else
    // the end of the palette rows to copy (m_uiArrayOffset <= it < m_uiArrayEnd). m_uiUploadEnd: the same for what the
    // GPU copy holds since the last Map(WRITE_DISCARD) - the array rows past it are undefined there.
    u32 m_uiArrayOffset;
    u32 m_uiArrayEnd;
    u32 m_uiFlushEnd;
    u32 m_uiUploadEnd;

    // Which render/main/dyn_cb_* bucket a flush of this buffer counts in (perf metrics only).
    enum : u8
    {
        KindOther,
        KindTransforms, // cbuffer dynamic_transforms
        KindGlobals, // $Globals
    };
    u8 m_kind;

    static const u32 lineSize = sizeof(Fvector4);

    //	Never try to copy objects of this class due to the pointer and autoptr members
    dx11ConstantBuffer(const dx11ConstantBuffer&);
    dx11ConstantBuffer& operator=(dx11ConstantBuffer&);
};

typedef resptr_core<dx11ConstantBuffer, resptr_base<dx11ConstantBuffer>> ref_cbuffer;
} // namespace xray::render::RENDER_NAMESPACE
