#pragma once

#include <vector>
#include <string>
#include <cstdio>
#include "TextureSnapshots.h"

// Read-only D3D8 observations, with optional bounded texture snapshots. No state overrides.
class RenderDiagnostics
{
public:
    static constexpr unsigned FrameCount = 3;
    static constexpr unsigned DrawLimit = 1024;
    enum class Phase { Idle, Armed, Capturing };

    bool active() const { return m_phase != Phase::Idle; }
    size_t size() const { return m_draws.size(); }
    void cancel() { m_phase = Phase::Idle; m_draws.clear(); m_snapshots.reset(); }

    void start(const char* root, unsigned zone, unsigned lod, unsigned fog, float distance, bool textures = false)
    {
        // Allocate before entering draw callbacks; capacity is bounded and reused.
        m_draws.clear();
        m_draws.reserve(FrameCount * DrawLimit);
        m_snapshots.reset(textures);
        m_root = root;
        m_zone = zone; m_lod = lod; m_fog = fog; m_distance = distance;
        m_frame = 0; m_seen = 0; m_skipped = 0;
        m_started = GetTickCount64();
        m_phase = Phase::Armed;
    }

    // Called before VanaView renders its UI. Starts at the next full Present interval.
    bool present(std::string& message)
    {
        if (!active()) return false;
        if (GetTickCount64() - m_started > 10000) {
            cancel(); message = "Diagnostic cancelled: capture timed out."; return true;
        }
        if (m_phase == Phase::Armed) { m_phase = Phase::Capturing; return false; }
        if (++m_frame < FrameCount) { m_seen = 0; return false; }
        m_phase = Phase::Idle;
        if (m_draws.empty()) message = "Diagnostic failed: no draw callbacks observed (all render targets included).";
        else save(message);
        m_draws.clear();
        m_snapshots.reset();
        return true;
    }

    void draw(IDirect3DDevice8* device, unsigned kind, D3DPRIMITIVETYPE primitive, unsigned count, bool sceneBackBuffer = false)
    {
        if (m_phase != Phase::Capturing || !device) return;
        const unsigned ordinal = m_seen++;
        if (ordinal >= DrawLimit) { ++m_skipped; return; }
        m_draws.emplace_back();
        auto& d = m_draws.back();
        d.frame = m_frame; d.ordinal = ordinal; d.kind = kind; d.primitive = primitive; d.count = count;
        d.sceneBackBuffer = sceneBackBuffer;
        IDirect3DSurface8* target = nullptr;
        IDirect3DSurface8* back = nullptr;
        d.targetHr = device->GetRenderTarget(&target);
        d.backHr = device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back);
        d.targetId = reinterpret_cast<uintptr_t>(target);
        d.backId = reinterpret_cast<uintptr_t>(back);
        if (SUCCEEDED(d.targetHr) && SUCCEEDED(d.backHr) && target && back)
            d.backMatch = target == back ? 1 : 0;
        if (target) {
            D3DSURFACE_DESC desc{};
            d.targetDescHr = target->GetDesc(&desc);
            if (SUCCEEDED(d.targetDescHr)) {
                d.targetWidth = desc.Width; d.targetHeight = desc.Height; d.targetFormat = desc.Format;
            }
            target->Release();
        }
        if (back) back->Release();
        d.vs = value([&](DWORD* v) { return device->GetVertexShader(v); });
        d.ps = value([&](DWORD* v) { return device->GetPixelShader(v); });
        const D3DRENDERSTATETYPE states[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC,
            D3DRS_ZBIAS, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE };
        for (unsigned i = 0; i < 6; ++i)
            d.render[i] = value([&](DWORD* v) { return device->GetRenderState(states[i], v); });
        d.projectionHr = device->GetTransform(D3DTS_PROJECTION, &d.projection);
        IDirect3DSurface8* depth = nullptr;
        d.depthHr = device->GetDepthStencilSurface(&depth);
        if (depth) {
            D3DSURFACE_DESC desc{};
            d.depthHr = depth->GetDesc(&desc);
            if (SUCCEEDED(d.depthHr)) d.depthFormat = desc.Format;
            depth->Release();
        }
        for (unsigned i = 0; i < 8; ++i) {
            auto& t = d.textures[i];
            const D3DTEXTURESTAGESTATETYPE samplerStates[] = { D3DTSS_MINFILTER, D3DTSS_MAGFILTER,
                D3DTSS_MIPFILTER, D3DTSS_MAXANISOTROPY, D3DTSS_MIPMAPLODBIAS,
                D3DTSS_MAXMIPLEVEL, D3DTSS_COLOROP, D3DTSS_ALPHAOP };
            for (unsigned j = 0; j < 8; ++j)
                t.sampler[j] = value([&](DWORD* v) { return device->GetTextureStageState(i, samplerStates[j], v); });
            IDirect3DBaseTexture8* texture = nullptr;
            t.hr = device->GetTexture(i, &texture);
            if (!texture) continue;
            t.id = reinterpret_cast<uintptr_t>(texture);
            t.type = texture->GetType(); t.levels = texture->GetLevelCount();
            if (t.type == D3DRTYPE_TEXTURE) {
                D3DSURFACE_DESC desc{};
                t.descHr = static_cast<IDirect3DTexture8*>(texture)->GetLevelDesc(0, &desc);
                if (SUCCEEDED(t.descHr)) { t.width = desc.Width; t.height = desc.Height;
                    t.format = desc.Format; t.usage = desc.Usage; t.pool = desc.Pool;
                    if (SUCCEEDED(d.render[0].hr) && d.render[0].data != D3DZB_FALSE)
                        m_snapshots.observe(static_cast<IDirect3DTexture8*>(texture), desc, t.levels);
                }
            }
            texture->Release();
        }
    }

private:
    struct Value { DWORD data = 0; HRESULT hr = E_FAIL; };
    template<class F> static Value value(F f) { Value v; v.hr = f(&v.data); return v; }
    struct Texture {
        uintptr_t id = 0;
        HRESULT hr = E_FAIL, descHr = E_NOTIMPL;
        unsigned type = 0, levels = 0, width = 0, height = 0, format = 0, usage = 0, pool = 0;
        Value sampler[8];
    };
    struct Draw {
        unsigned frame = 0, ordinal = 0, kind = 0, primitive = 0, count = 0, depthFormat = 0;
        bool sceneBackBuffer = false;
        int backMatch = -1;
        uintptr_t targetId = 0, backId = 0;
        unsigned targetWidth = 0, targetHeight = 0, targetFormat = 0;
        HRESULT targetHr = E_FAIL, backHr = E_FAIL, targetDescHr = E_FAIL;
        HRESULT depthHr = E_FAIL, projectionHr = E_FAIL;
        Value vs, ps, render[6];
        D3DMATRIX projection{};
        Texture textures[8];
    };

    void save(std::string& message)
    {
        const std::string dir = m_root + "\\logs";
        if (!CreateDirectoryA(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            message = "Diagnostic failed: cannot create logs directory."; return;
        }
        SYSTEMTIME now{}; GetLocalTime(&now);
        char name[160];
        std::snprintf(name, sizeof(name), "\\VanaView-diag-%04u%02u%02u-%02u%02u%02u-%03u-%lu-%llu.csv",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
            now.wMilliseconds, GetCurrentProcessId(), GetTickCount64());
        const std::string path = dir + name;
        HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { message = "Diagnostic failed: cannot create CSV."; return; }
        struct FileGuard {
            HANDLE handle;
            ~FileGuard() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
        } guard{file};
        bool ok = true;
        auto write = [&](const std::string& s) {
            DWORD written = 0;
            if (ok && (!WriteFile(file, s.data(), static_cast<DWORD>(s.size()), &written, nullptr)
                || written != s.size())) ok = false;
        };
        char metadata[512];
        std::snprintf(metadata, sizeof(metadata),
            "# VanaView 1.22; schema=2; zone=%u; lod=%u; fog=%u; distance=%.6f; frames=3; draw_limit_per_frame=1024; skipped_draws=%u\n",
            m_zone, m_lod, m_fog, m_distance, m_skipped);
        write(metadata);
        std::string header = "frame,draw,kind,primitive,primitive_count,scene_backbuffer_hint,target_id,backbuffer_id,backbuffer_pointer_match,target_width,target_height,target_format,target_hr,backbuffer_hr,target_desc_hr,depth_format,depth_hr,projection_hr";
        for (const char* n : { "vs", "ps", "zenable", "zwrite", "zfunc", "zbias", "blend", "alpha_test" })
            header += std::string(",") + n + "," + n + "_hr";
        for (unsigned i = 0; i < 16; ++i) header += ",projection_" + std::to_string(i);
        header += ",stage,texture_id,texture_hr,type,levels,width,height,format,usage,pool,desc_hr";
        for (const char* n : { "min", "mag", "mip", "anisotropy", "lod_bias_bits", "max_mip", "colorop", "alphaop" })
            header += std::string(",") + n + "," + n + "_hr";
        write(header + "\n");
        for (const auto& d : m_draws) {
            std::string prefix;
            auto append = [](std::string& s, auto v) { if (!s.empty()) s += ','; s += std::to_string(v); };
            append(prefix, d.frame); append(prefix, d.ordinal); append(prefix, d.kind);
            append(prefix, d.primitive); append(prefix, d.count);
            append(prefix, d.sceneBackBuffer); append(prefix, d.targetId); append(prefix, d.backId);
            append(prefix, d.backMatch); append(prefix, d.targetWidth); append(prefix, d.targetHeight);
            append(prefix, d.targetFormat); append(prefix, d.targetHr); append(prefix, d.backHr);
            append(prefix, d.targetDescHr); append(prefix, d.depthFormat);
            append(prefix, d.depthHr); append(prefix, d.projectionHr);
            for (const auto& v : {d.vs, d.ps}) { append(prefix, v.data); append(prefix, v.hr); }
            for (const auto& v : d.render) { append(prefix, v.data); append(prefix, v.hr); }
            for (unsigned r = 0; r < 4; ++r)
                for (unsigned c = 0; c < 4; ++c) append(prefix, d.projection.m[r][c]);
            for (unsigned i = 0; i < 8; ++i) {
                const auto& t = d.textures[i];
                std::string row = prefix;
                append(row, i); append(row, t.id); append(row, t.hr); append(row, t.type);
                append(row, t.levels); append(row, t.width); append(row, t.height);
                append(row, t.format); append(row, t.usage); append(row, t.pool); append(row, t.descHr);
                for (const auto& v : t.sampler) { append(row, v.data); append(row, v.hr); }
                write(row + "\n");
            }
        }
        if (!CloseHandle(file)) ok = false;
        guard.handle = INVALID_HANDLE_VALUE;
        message = ok ? "Diagnostic saved: " + path : "Diagnostic write failed (partial CSV): " + path;
        if (ok) message += m_snapshots.save(path);
    }

    TextureSnapshots m_snapshots;
    Phase m_phase = Phase::Idle;
    std::vector<Draw> m_draws;
    std::string m_root;
    unsigned m_frame = 0, m_seen = 0, m_skipped = 0, m_zone = 0, m_lod = 0, m_fog = 0;
    float m_distance = 0;
    ULONGLONG m_started = 0;
};
