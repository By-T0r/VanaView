#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <cstring>

// Optional, bounded level-zero copies for offline identification; never a live replacement cache.
class TextureSnapshots
{
public:
    static constexpr size_t MaxTextures = 128;
    static constexpr size_t MaxBytes = 16 * 1024 * 1024;
    static constexpr size_t MaxTextureBytes = 1024 * 1024;

    void reset(bool enabled = false)
    {
        m_enabled = enabled; m_items.clear(); m_bytes = 0; m_skipped = 0;
        if (enabled) m_items.reserve(MaxTextures);
    }

    void observe(IDirect3DTexture8* texture, const D3DSURFACE_DESC& desc, UINT levels)
    {
        if (!m_enabled || !texture || levels != 1 || desc.Pool != D3DPOOL_MANAGED || desc.Usage != 0)
            return;
        const unsigned blockBytes = desc.Format == D3DFMT_DXT1 ? 8 :
            (desc.Format == D3DFMT_DXT3 || desc.Format == D3DFMT_DXT5) ? 16 : 0;
        if (!blockBytes || !desc.Width || !desc.Height || desc.Width > 4096 || desc.Height > 4096) return;
        const uintptr_t id = reinterpret_cast<uintptr_t>(texture);
        for (const auto& item : m_items) if (item.id == id) return;
        if (m_items.size() >= MaxTextures) { ++m_skipped; return; }
        const size_t pitch = ((desc.Width + 3) / 4) * blockBytes;
        const size_t rows = (desc.Height + 3) / 4;
        const size_t bytes = pitch * rows;
        m_items.emplace_back();
        auto& item = m_items.back();
        item.id = id; item.width = desc.Width; item.height = desc.Height; item.format = desc.Format;
        if (bytes > MaxTextureBytes || bytes > MaxBytes - m_bytes) {
            item.hr = HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY); ++m_skipped; return;
        }
        // Allocate before locking; failures are local to the optional export.
        try { item.data.resize(bytes); }
        catch (...) { item.hr = E_OUTOFMEMORY; return; }
        D3DLOCKED_RECT locked{};
        item.hr = texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(item.hr)) { std::vector<uint8_t>().swap(item.data); return; }
        if (!locked.pBits || locked.Pitch < 0 || static_cast<size_t>(locked.Pitch) < pitch) {
            item.hr = E_FAIL;
        } else {
            for (size_t row = 0; row < rows; ++row)
                std::memcpy(item.data.data() + row * pitch,
                    static_cast<const uint8_t*>(locked.pBits) + row * locked.Pitch, pitch);
        }
        const HRESULT unlock = texture->UnlockRect(0);
        if (FAILED(unlock)) item.hr = unlock;
        if (FAILED(item.hr)) { std::vector<uint8_t>().swap(item.data); return; }
        m_bytes += bytes;
    }

    std::string save(const std::string& csvPath) const
    {
        if (!m_enabled) return "";
        const std::string dir = csvPath + ".textures";
        if (!CreateDirectoryA(dir.c_str(), nullptr)) return " Texture export failed: cannot create directory.";
        std::string manifest = "texture_id,width,height,format,read_hr,file,write_ok\n";
        unsigned saved = 0;
        for (const auto& item : m_items) {
            std::string name;
            bool written = false;
            if (SUCCEEDED(item.hr) && !item.data.empty()) {
                name = "texture-" + std::to_string(item.id) + ".dds";
                // DDS magic + 124-byte legacy header, followed by packed DXT blocks.
                uint32_t header[32]{};
                header[0] = 0x20534444; header[1] = 124; header[2] = 0x81007;
                header[3] = item.height; header[4] = item.width;
                header[5] = static_cast<uint32_t>(item.data.size());
                header[19] = 32; header[20] = 4; header[21] = item.format; header[27] = 0x1000;
                written = writeFile(dir + "\\" + name, header, sizeof(header), item.data.data(), item.data.size());
                if (written) ++saved;
            }
            manifest += std::to_string(item.id) + "," + std::to_string(item.width) + "," +
                std::to_string(item.height) + "," + std::to_string(item.format) + "," +
                std::to_string(item.hr) + "," + name + "," + (written ? "1\n" : "0\n");
        }
        const bool ok = writeFile(dir + "\\manifest.csv", manifest.data(), manifest.size(), nullptr, 0);
        return " Textures: " + std::to_string(saved) + "/" + std::to_string(m_items.size()) +
            " saved; skipped encounters=" + std::to_string(m_skipped) +
            (ok ? ". See CSV.textures folder." : ". Manifest write FAILED.");
    }

private:
    static bool writeFile(const std::string& path, const void* a, size_t aSize, const void* b, size_t bSize)
    {
        HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        bool ok = WriteFile(file, a, static_cast<DWORD>(aSize), &written, nullptr) && written == aSize;
        if (ok && bSize) ok = WriteFile(file, b, static_cast<DWORD>(bSize), &written, nullptr) && written == bSize;
        if (!CloseHandle(file)) ok = false;
        return ok;
    }
    struct Item {
        uintptr_t id = 0;
        unsigned width = 0, height = 0, format = 0;
        HRESULT hr = E_FAIL;
        std::vector<uint8_t> data;
    };
    bool m_enabled = false;
    size_t m_bytes = 0, m_skipped = 0;
    std::vector<Item> m_items;
};
