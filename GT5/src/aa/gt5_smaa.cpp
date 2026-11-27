/*
 * gt5_smaa.cpp - SMAA 1x: the game's own anti-aliasing (LD_AA=smaa, the menu's "SMAA"), Direct3D 12.
 *
 * Jorge Jimenez et al.'s reference SMAA (third_party/smaa: SMAA.hlsl, AreaTex.h, SearchTex.h; MIT,
 * unmodified) as three compute passes over the 3D scene, before the game's own exposure, bloom and HUD:
 *   1. luma edge detection     the scene              -> edges (R8G8)
 *   2. blending weights        edges + the two lookup textures -> weights (R8G8B8A8)
 *   3. neighbourhood blending  the scene + weights    -> the picture the game's copy reads
 * Unlike DLSS and FSR it works on each frame alone: no jitter, no history, no motion vectors, so nothing
 * smears in motion. It smooths the stair steps the game's own sample grid leaves (1080p mode's 2x1 grid
 * leaves the near-horizontal edges of the PS3 picture as they are).
 *
 * It is the third back end behind gt5_upscale.h (the renderer's upscaler interface; rsx_upscale.c,
 * RSX_UPSCALE_SMAA), built into the game instead of a plug-in DLL: gt5_smaa_get() here, and a stand-in
 * that returns NULL in the runtime library (libs/video/rsx_smaa_none.c) for builds without this file.
 *   optimal()   the render size is the output's: SMAA never changes the size
 *   create()    the three pipelines (compiled the first time, then read from <logs>\smaa_shaders.bin),
 *               the lookup textures (uploaded once, recorded into the given list) and the textures for
 *               this size
 *   evaluate()  copies the frame's part of the scene into a texture of exactly that size (the scene's
 *               surface is larger than its picture, and every pass then reads textures of one size),
 *               then records the three passes. The output is the renderer's, R8G8B8A8_UNORM.
 * Everything runs on the renderer's thread; create() and destroy() only after the renderer has waited
 * for its lists (rsx_live_draw.c ld_up_make / ld_up_release), so descriptors are rewritten safely.
 */
#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "gt5_upscale.h"
#include "AreaTex.h"
#include "SearchTex.h"
#include "smaa_hlsl.h"   /* k_smaa_hlsl[]: SMAA.hlsl's bytes and a closing 0 (written by GT5/CMakeLists.txt) */

namespace {

/* The one descriptor table: the passes' textures, then what they write. */
enum { D_COLOR = 0, D_EDGES, D_BLEND, D_AREA, D_SEARCH, D_EDGES_W, D_BLEND_W, D_OUT_W, D_COUNT };
enum { P_EDGES = 0, P_WEIGHTS, P_BLEND, P_COUNT };
const char* const k_entry[P_COUNT] = { "smaa_edges_cs", "smaa_weights_cs", "smaa_blend_cs" };

struct State {
    ID3D12Device* dev;
    pD3DCompile compile;
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize;
    wchar_t cache[MAX_PATH + 32];      /* <logs>\smaa_shaders.bin, "" = none */
    int pipes;                         /* 1 made, -1 could not be made (not tried again) */
    ID3D12RootSignature* root;
    ID3D12PipelineState* pso[P_COUNT];
    ID3D12DescriptorHeap* heap;
    UINT inc;
    ID3D12Resource* area;              /* the lookup textures, NON_PIXEL_SHADER_RESOURCE once uploaded */
    ID3D12Resource* search;
    ID3D12Resource* upload;            /* their upload buffer: kept until shutdown (the list may not have run) */
    ID3D12Resource* copy;              /* the scene's picture, w x h, R8G8B8A8_UNORM */
    ID3D12Resource* edges;             /* w x h, edges_fmt */
    ID3D12Resource* blend;             /* w x h, R8G8B8A8_UNORM */
    DXGI_FORMAT edges_fmt;
    unsigned w, h;
    ID3D12Resource* out_seen;          /* the output whose view is in the table */
    char err[300];
};
State s;

void fail(const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vsnprintf(s.err, sizeof s.err, fmt, a);
    va_end(a);
}

template <class T> void drop(T*& p)
{
    if (p) { p->Release(); p = nullptr; }
}

D3D12_CPU_DESCRIPTOR_HANDLE slot(UINT i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = s.heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)i * s.inc;
    return h;
}

void view_srv(UINT i, ID3D12Resource* r, DXGI_FORMAT f)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = f;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    s.dev->CreateShaderResourceView(r, &d, slot(i));
}

void view_uav(UINT i, ID3D12Resource* r, DXGI_FORMAT f)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = f;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    s.dev->CreateUnorderedAccessView(r, nullptr, &d, slot(i));
}

ID3D12Resource* make_tex(unsigned w, unsigned h, DXGI_FORMAT f, bool writable, D3D12_RESOURCE_STATES st)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.Format = f; rd.SampleDesc.Count = 1;
    rd.Flags = writable ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* r = nullptr;
    if (FAILED(s.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}

void transition(D3D12_RESOURCE_BARRIER& b, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
}

bool typed_store(DXGI_FORMAT f)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {};
    fs.Format = f;
    if (FAILED(s.dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs))) return false;
    return (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) && (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
}

/* ---- the shaders ---------------------------------------------------------------------------------
 * SMAA.hlsl with its own porting macros (SMAA_CUSTOM_SL): every read is a SampleLevel, as a compute
 * shader must; its pixel shaders' "discard" (no edge) becomes "no edge" written; the vertex shaders'
 * offsets are worked out per pixel. Preset High (threshold 0.1, 16 search steps, 8 diagonal steps,
 * corner rounding 25). */
const char k_head[] =
    "#define SMAA_CUSTOM_SL 1\n"
    "#define SMAA_PRESET_HIGH 1\n"
    "SamplerState LinearSampler : register(s0);\n"
    "SamplerState PointSampler : register(s1);\n"
    "cbuffer smaa_consts : register(b0) { float4 g_smaa_metrics; uint2 g_smaa_size; uint2 g_smaa_pad; };\n"
    "#define SMAA_RT_METRICS g_smaa_metrics\n"
    "#define SMAATexture2D(tex) Texture2D tex\n"
    "#define SMAATexturePass2D(tex) tex\n"
    "#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)\n"
    "#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)\n"
    "#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)\n"
    "#define SMAASample(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)\n"
    "#define SMAASamplePoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)\n"
    "#define SMAASampleOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)\n"
    "#define SMAA_FLATTEN [flatten]\n"
    "#define SMAA_BRANCH [branch]\n"
    "#line 1 \"SMAA.hlsl\"\n";

const char k_tail[] =
    "\n#line 1 \"gt5_smaa.cpp\"\n"
    "Texture2D smaa_color  : register(t0);\n"
    "Texture2D smaa_edges  : register(t1);\n"
    "Texture2D smaa_blend  : register(t2);\n"
    "Texture2D smaa_area   : register(t3);\n"
    "Texture2D smaa_search : register(t4);\n"
    "RWTexture2D<unorm float4> smaa_edges_w : register(u0);\n"
    "RWTexture2D<unorm float4> smaa_blend_w : register(u1);\n"
    "RWTexture2D<unorm float4> smaa_out_w   : register(u2);\n"
    "float2 smaa_uv(uint2 p) { return (float2(p) + 0.5) * SMAA_RT_METRICS.xy; }\n"
    "[numthreads(8, 8, 1)]\n"
    "void smaa_edges_cs(uint3 id : SV_DispatchThreadID) {\n"
    "    if (any(id.xy >= g_smaa_size)) return;\n"
    "    float2 uv = smaa_uv(id.xy);\n"
    "    float4 offset[3];\n"
    "    SMAAEdgeDetectionVS(uv, offset);\n"
    "    smaa_edges_w[id.xy] = float4(SMAALumaEdgeDetectionPS(uv, offset, smaa_color), 0.0, 0.0);\n"
    "}\n"
    "[numthreads(8, 8, 1)]\n"
    "void smaa_weights_cs(uint3 id : SV_DispatchThreadID) {\n"
    "    if (any(id.xy >= g_smaa_size)) return;\n"
    "    float2 uv = smaa_uv(id.xy), pix;\n"
    "    float4 offset[3];\n"
    "    SMAABlendingWeightCalculationVS(uv, pix, offset);\n"
    "    smaa_blend_w[id.xy] = SMAABlendingWeightCalculationPS(uv, pix, offset, smaa_edges, smaa_area, smaa_search, float4(0.0, 0.0, 0.0, 0.0));\n"
    "}\n"
    "[numthreads(8, 8, 1)]\n"
    "void smaa_blend_cs(uint3 id : SV_DispatchThreadID) {\n"
    "    if (any(id.xy >= g_smaa_size)) return;\n"
    "    float2 uv = smaa_uv(id.xy);\n"
    "    float4 offset;\n"
    "    SMAANeighborhoodBlendingVS(uv, offset);\n"
    "    smaa_out_w[id.xy] = SMAANeighborhoodBlendingPS(uv, offset, smaa_color, smaa_blend);\n"
    "}\n";

std::string shader_source()
{
    std::string body((const char*)k_smaa_hlsl, sizeof k_smaa_hlsl - 1u);
    /* A compute shader has no discard: SMAA's edge detection then writes "no edge" (its return type
     * is float2 in all three places). */
    const std::string from = "discard;", to = "return float2(0.0, 0.0);";
    for (size_t at = body.find(from); at != std::string::npos; at = body.find(from, at + to.size()))
        body.replace(at, from.size(), to);
    return std::string(k_head) + body + k_tail;
}

unsigned long long fnv1a(const void* p, size_t n, unsigned long long h = 1469598103934665603ull)
{
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

/* The compiled passes, kept in <logs>\smaa_shaders.bin: "GT5SMAA1", the source's hash, then each
 * pass's size and bytes. A file of another source (a game update changed it) is compiled anew. */
const char k_magic[8] = { 'G', 'T', '5', 'S', 'M', 'A', 'A', '1' };

bool cache_read(unsigned long long hash, std::string blobs[P_COUNT])
{
    if (!s.cache[0]) return false;
    FILE* f = _wfopen(s.cache, L"rb");
    if (!f) return false;
    char magic[8];
    unsigned long long h = 0;
    bool ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, k_magic, 8) && fread(&h, sizeof h, 1, f) == 1 && h == hash;
    for (int i = 0; ok && i < P_COUNT; i++) {
        unsigned n = 0;
        ok = fread(&n, sizeof n, 1, f) == 1 && n > 0 && n < (8u << 20);
        if (ok) { blobs[i].resize(n); ok = fread(&blobs[i][0], 1, n, f) == n; }
    }
    fclose(f);
    return ok;
}

void cache_write(unsigned long long hash, const std::string blobs[P_COUNT])
{
    if (!s.cache[0]) return;
    FILE* f = _wfopen(s.cache, L"wb");
    if (!f) return;
    bool ok = fwrite(k_magic, 1, 8, f) == 8 && fwrite(&hash, sizeof hash, 1, f) == 1;
    for (int i = 0; ok && i < P_COUNT; i++) {
        const unsigned n = (unsigned)blobs[i].size();
        ok = fwrite(&n, sizeof n, 1, f) == 1 && fwrite(blobs[i].data(), 1, n, f) == n;
    }
    fclose(f);
    if (!ok) DeleteFileW(s.cache);
}

bool compile_passes(std::string blobs[P_COUNT])
{
    const std::string src = shader_source();
    const unsigned long long hash = fnv1a(src.data(), src.size());
    if (cache_read(hash, blobs)) {
        fprintf(stderr, "[smaa] shaders read from smaa_shaders.bin\n");
        return true;
    }
    LARGE_INTEGER f0, t0, t1;
    QueryPerformanceFrequency(&f0); QueryPerformanceCounter(&t0);
    for (int i = 0; i < P_COUNT; i++) {
        ID3DBlob* code = nullptr;
        ID3DBlob* errs = nullptr;
        const HRESULT hr = s.compile(src.data(), src.size(), "SMAA.hlsl", nullptr, nullptr, k_entry[i], "cs_5_0",
                                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errs);
        if (FAILED(hr) || !code) {
            const char* e = errs ? (const char*)errs->GetBufferPointer() : "";
            fail("SMAA's %s could not be compiled (0x%08lX): %.200s", k_entry[i], (unsigned long)hr, e);
            fprintf(stderr, "[smaa] %s\n", s.err);
            drop(code); drop(errs);
            return false;
        }
        blobs[i].assign((const char*)code->GetBufferPointer(), code->GetBufferSize());
        drop(code); drop(errs);
    }
    QueryPerformanceCounter(&t1);
    fprintf(stderr, "[smaa] shaders compiled in %.0f ms\n", (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f0.QuadPart);
    cache_write(hash, blobs);
    return true;
}

/* Root signature, the three pipelines and the descriptor table: once per run. */
bool make_pipelines()
{
    if (s.pipes > 0) return true;
    if (s.pipes < 0) { if (!s.err[0]) fail("SMAA's shaders could not be made"); return false; }
    s.pipes = -1;
    std::string blobs[P_COUNT];
    if (!compile_passes(blobs)) return false;

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 5;
    ranges[0].OffsetInDescriptorsFromTableStart = D_COLOR;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 3;
    ranges[1].OffsetInDescriptorsFromTableStart = D_EDGES_W;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 8;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC smp[2] = {};
    for (int i = 0; i < 2; i++) {
        smp[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_POINT;
        smp[i].AddressU = smp[i].AddressV = smp[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        smp[i].MaxLOD = D3D12_FLOAT32_MAX;
        smp[i].ShaderRegister = (UINT)i;
        smp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rd = {};
    rd.NumParameters = 2; rd.pParameters = params;
    rd.NumStaticSamplers = 2; rd.pStaticSamplers = smp;
    ID3DBlob* sig = nullptr;
    ID3DBlob* errs = nullptr;
    if (FAILED(s.serialize(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &errs)) || !sig ||
        FAILED(s.dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&s.root)))) {
        fail("SMAA's root signature could not be made%s%.150s", errs ? ": " : "", errs ? (const char*)errs->GetBufferPointer() : "");
        drop(sig); drop(errs);
        return false;
    }
    drop(sig); drop(errs);
    for (int i = 0; i < P_COUNT; i++) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = s.root;
        pd.CS.pShaderBytecode = blobs[i].data();
        pd.CS.BytecodeLength = blobs[i].size();
        const HRESULT hr = s.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&s.pso[i]));
        if (FAILED(hr)) {
            fail("SMAA's %s pipeline could not be made (0x%08lX)", k_entry[i], (unsigned long)hr);
            if (s.cache[0]) DeleteFileW(s.cache);   /* in case the file was the problem */
            return false;
        }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = D_COUNT;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(s.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.heap)))) { fail("no descriptor heap for SMAA"); return false; }
    s.inc = s.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    s.pipes = 1;
    return true;
}

/* The two lookup textures, uploaded once: the copies are recorded into the list create() was given. */
bool make_luts(ID3D12GraphicsCommandList* list)
{
    s.area = make_tex(AREATEX_WIDTH, AREATEX_HEIGHT, DXGI_FORMAT_R8G8_UNORM, false, D3D12_RESOURCE_STATE_COPY_DEST);
    s.search = make_tex(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM, false, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!s.area || !s.search) { drop(s.area); drop(s.search); fail("no lookup textures for SMAA"); return false; }
    ID3D12Resource* tex[2] = { s.area, s.search };
    const unsigned char* data[2] = { areaTexBytes, searchTexBytes };
    const UINT pitch[2] = { AREATEX_PITCH, SEARCHTEX_PITCH };
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[2];
    UINT rows[2];
    UINT64 row_bytes[2], total[2];
    UINT64 at = 0;
    for (int i = 0; i < 2; i++) {
        const D3D12_RESOURCE_DESC d = tex[i]->GetDesc();
        s.dev->GetCopyableFootprints(&d, 0, 1, at, &fp[i], &rows[i], &row_bytes[i], &total[i]);
        at = (fp[i].Offset + total[i] + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~(UINT64)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    }
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = at; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    unsigned char* map = nullptr;
    const D3D12_RANGE none = { 0, 0 };
    if (FAILED(s.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(&s.upload))) ||
        FAILED(s.upload->Map(0, &none, (void**)&map)) || !map) {
        drop(s.upload); drop(s.area); drop(s.search);
        fail("no upload buffer for SMAA's lookup textures");
        return false;
    }
    for (int i = 0; i < 2; i++)
        for (UINT y = 0; y < rows[i]; y++)
            memcpy(map + fp[i].Offset + (UINT64)y * fp[i].Footprint.RowPitch, data[i] + (size_t)y * pitch[i], pitch[i]);
    s.upload->Unmap(0, nullptr);
    D3D12_RESOURCE_BARRIER b[2];
    for (int i = 0; i < 2; i++) {
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = tex[i]; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = s.upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp[i];
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(b[i], tex[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    list->ResourceBarrier(2, b);
    view_srv(D_AREA, s.area, DXGI_FORMAT_R8G8_UNORM);
    view_srv(D_SEARCH, s.search, DXGI_FORMAT_R8_UNORM);
    return true;
}

void drop_sized()
{
    drop(s.copy); drop(s.edges); drop(s.blend);
    s.w = s.h = 0;
    s.out_seen = nullptr;
}

/* ---- gt5_upscale_api ---------------------------------------------------------------------------- */

int smaa_init(ID3D12Device* device, const wchar_t* dir, gt5_upscale_caps* caps)
{
    memset(caps, 0, sizeof *caps);
    snprintf(caps->name, sizeof caps->name, "%s", "SMAA");
    if (!device) { snprintf(caps->message, sizeof caps->message, "%s", "no Direct3D 12 device"); return 0; }
    s.dev = device;
    s.cache[0] = 0;
    if (dir && dir[0]) _snwprintf(s.cache, MAX_PATH + 32, L"%ls\\smaa_shaders.bin", dir);
    s.cache[MAX_PATH + 31] = 0;
    HMODULE c = LoadLibraryW(L"d3dcompiler_47.dll");
    s.compile = c ? (pD3DCompile)(void*)GetProcAddress(c, "D3DCompile") : nullptr;
    HMODULE d = GetModuleHandleW(L"d3d12.dll");
    s.serialize = d ? (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(void*)GetProcAddress(d, "D3D12SerializeRootSignature") : nullptr;
    if (!s.compile || !s.serialize) {
        snprintf(caps->message, sizeof caps->message, "%s",
                 !s.compile ? "Windows' shader compiler (d3dcompiler_47.dll) could not be loaded" : "Direct3D 12 is missing a function");
        return 1;
    }
    s.edges_fmt = typed_store(DXGI_FORMAT_R8G8_UNORM) ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    caps->available = 1;
    return 1;
}

int smaa_optimal(unsigned out_w, unsigned out_h, gt5_upscale_mode mode, unsigned* render_w, unsigned* render_h)
{
    (void)mode;
    *render_w = out_w;
    *render_h = out_h;
    return 1;
}

int smaa_create(ID3D12GraphicsCommandList* list, unsigned render_w, unsigned render_h, unsigned out_w, unsigned out_h,
                gt5_upscale_mode mode, unsigned flags, int preset)
{
    (void)mode; (void)flags; (void)preset;
    s.err[0] = 0;
    if (!s.dev || !list) { fail("%s", "SMAA was not started"); return 0; }
    if (render_w != out_w || render_h != out_h) {
        fail("SMAA keeps the picture's size (asked for %ux%u to %ux%u)", render_w, render_h, out_w, out_h);
        return 0;
    }
    if (render_w < 16u || render_h < 16u || render_w > 16384u || render_h > 16384u) {
        fail("SMAA cannot work on a %ux%u picture", render_w, render_h);
        return 0;
    }
    if (!make_pipelines()) return 0;
    if (!s.area && !make_luts(list)) return 0;
    if (!s.copy || s.w != render_w || s.h != render_h) {
        drop_sized();
        s.copy = make_tex(render_w, render_h, DXGI_FORMAT_R8G8B8A8_UNORM, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        s.edges = make_tex(render_w, render_h, s.edges_fmt, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        s.blend = make_tex(render_w, render_h, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!s.copy || !s.edges || !s.blend) {
            drop_sized();
            fail("no textures of %ux%u for SMAA could be made", render_w, render_h);
            return 0;
        }
        s.w = render_w; s.h = render_h;
        view_srv(D_COLOR, s.copy, DXGI_FORMAT_R8G8B8A8_UNORM);
        view_srv(D_EDGES, s.edges, s.edges_fmt);
        view_srv(D_BLEND, s.blend, DXGI_FORMAT_R8G8B8A8_UNORM);
        view_uav(D_EDGES_W, s.edges, s.edges_fmt);
        view_uav(D_BLEND_W, s.blend, DXGI_FORMAT_R8G8B8A8_UNORM);
        fprintf(stderr, "[smaa] made for %ux%u (edges %s)\n", render_w, render_h,
                s.edges_fmt == DXGI_FORMAT_R8G8_UNORM ? "R8G8" : "R8G8B8A8");
    }
    s.out_seen = nullptr;   /* the output's view is written by the next evaluate */
    return 1;
}

int smaa_evaluate(const gt5_upscale_frame* f)
{
    if (!f || !f->list || !f->color || !f->output) { fail("%s", "SMAA was given nothing to work on"); return 0; }
    if (!s.copy || f->render_w != s.w || f->render_h != s.h) {
        fail("SMAA is made for %ux%u, the frame is %ux%u", s.w, s.h, f->render_w, f->render_h);
        return 0;
    }
    const D3D12_RESOURCE_DESC cd = f->color->GetDesc();
    const D3D12_RESOURCE_DESC od = f->output->GetDesc();
    if (cd.Format != DXGI_FORMAT_R8G8B8A8_UNORM || cd.Width < s.w || cd.Height < s.h) {
        fail("the scene is not an 8-bit picture of at least %ux%u", s.w, s.h);
        return 0;
    }
    if (od.Width < s.w || od.Height < s.h) { fail("the output is smaller than %ux%u", s.w, s.h); return 0; }
    if (f->output != s.out_seen) {
        view_uav(D_OUT_W, f->output, DXGI_FORMAT_R8G8B8A8_UNORM);
        s.out_seen = f->output;
    }
    ID3D12GraphicsCommandList* l = f->list;
    const D3D12_RESOURCE_STATES NPS = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_BARRIER b[3];

    /* the frame's part of the scene, into a texture of exactly its size */
    transition(b[0], f->color, NPS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(b[1], s.copy, NPS, D3D12_RESOURCE_STATE_COPY_DEST);
    l->ResourceBarrier(2, b);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = s.copy;   dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = f->color; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box = { 0, 0, 0, s.w, s.h, 1 };
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    transition(b[0], f->color, D3D12_RESOURCE_STATE_COPY_SOURCE, NPS);
    transition(b[1], s.copy, D3D12_RESOURCE_STATE_COPY_DEST, NPS);
    transition(b[2], s.edges, NPS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->ResourceBarrier(3, b);

    ID3D12DescriptorHeap* heaps[1] = { s.heap };
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(s.root);
    UINT consts[8];
    const float metrics[4] = { 1.0f / (float)s.w, 1.0f / (float)s.h, (float)s.w, (float)s.h };
    memcpy(consts, metrics, sizeof metrics);
    consts[4] = s.w; consts[5] = s.h; consts[6] = consts[7] = 0;
    l->SetComputeRoot32BitConstants(0, 8, consts, 0);
    l->SetComputeRootDescriptorTable(1, s.heap->GetGPUDescriptorHandleForHeapStart());
    const UINT gx = (s.w + 7u) / 8u, gy = (s.h + 7u) / 8u;

    l->SetPipelineState(s.pso[P_EDGES]);
    l->Dispatch(gx, gy, 1);
    transition(b[0], s.edges, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NPS);
    transition(b[1], s.blend, NPS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->ResourceBarrier(2, b);

    l->SetPipelineState(s.pso[P_WEIGHTS]);
    l->Dispatch(gx, gy, 1);
    transition(b[0], s.blend, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NPS);
    l->ResourceBarrier(1, b);

    l->SetPipelineState(s.pso[P_BLEND]);   /* into the output, which is in UNORDERED_ACCESS as handed over */
    l->Dispatch(gx, gy, 1);
    return 1;
}

void smaa_destroy(void)
{
    drop_sized();
}

void smaa_shutdown(ID3D12Device* device)
{
    (void)device;
    drop_sized();
    for (int i = 0; i < P_COUNT; i++) drop(s.pso[i]);
    drop(s.root); drop(s.heap); drop(s.area); drop(s.search); drop(s.upload);
    s.pipes = 0;
    s.dev = nullptr;
}

const char* smaa_last_error(void) { return s.err; }

const gt5_upscale_api k_api = {
    GT5_UPSCALE_API_VERSION,
    smaa_init, smaa_optimal, smaa_create, smaa_evaluate, smaa_destroy, smaa_shutdown, smaa_last_error,
    nullptr, nullptr, nullptr, nullptr, nullptr,   /* no frame generator */
};

} // namespace

/* The built-in back end (rsx_upscale.c). The runtime library's stand-in (rsx_smaa_none.c) returns NULL. */
extern "C" const gt5_upscale_api* gt5_smaa_get(void) { return &k_api; }
#endif /* _WIN32 */
