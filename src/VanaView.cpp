#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "Ashita.h"
#include "TerrainMipmaps.h"
#include "FogDistanceScale.h"
#include "NormalShadows.h"
#include "SceneryMipmaps.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <mutex>
#include <optional>
#include <string_view>

#pragma comment(linker, "/EXPORT:expCreatePlugin=_expCreatePlugin@4")
#pragma comment(linker, "/EXPORT:expDestroyPlugin=_expDestroyPlugin@4")
#pragma comment(linker, "/EXPORT:expGetInterfaceVersion=_expGetInterfaceVersion@0")

namespace
{
    struct BytesPatchSite
    {
        uintptr_t address = 0;
        uint8_t original[16]{};
        uint32_t length = 0;
        bool patched = false;
    };

    template<typename T>
    bool safe_read(const uintptr_t address, T& value)
    {
        __try
        {
            value = *reinterpret_cast<const T*>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    template<typename T>
    bool safe_write(const uintptr_t address, const T& value)
    {
        DWORD oldProtect = 0;
        if (!::VirtualProtect(reinterpret_cast<void*>(address), sizeof(T), PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;

        __try
        {
            *reinterpret_cast<T*>(address) = value;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            DWORD unused = 0;
            ::VirtualProtect(reinterpret_cast<void*>(address), sizeof(T), oldProtect, &unused);
            return false;
        }

        DWORD unused = 0;
        ::VirtualProtect(reinterpret_cast<void*>(address), sizeof(T), oldProtect, &unused);
        return true;
    }

    bool get_module_range(const char* moduleName, uintptr_t& base, uint32_t& size)
    {
        const auto module = ::GetModuleHandleA(moduleName);
        if (module == nullptr)
            return false;

        IMAGE_DOS_HEADER dos{};
        if (!safe_read(reinterpret_cast<uintptr_t>(module), dos) || dos.e_magic != IMAGE_DOS_SIGNATURE)
            return false;

        IMAGE_NT_HEADERS32 nt{};
        if (!safe_read(reinterpret_cast<uintptr_t>(module) + static_cast<uintptr_t>(dos.e_lfanew), nt) ||
            nt.Signature != IMAGE_NT_SIGNATURE)
            return false;

        base = reinterpret_cast<uintptr_t>(module);
        size = nt.OptionalHeader.SizeOfImage;
        return size != 0;
    }

    uintptr_t find_bytes(const uintptr_t base, const uint32_t size, const uint8_t* pattern, const size_t length)
    {
        if (base == 0 || size < length)
            return 0;

        for (uint32_t i = 0; i <= size - length; ++i)
        {
            bool match = true;
            __try
            {
                for (size_t j = 0; j < length; ++j)
                {
                    if (*reinterpret_cast<const uint8_t*>(base + i + j) != pattern[j])
                    {
                        match = false;
                        break;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                match = false;
            }

            if (match)
                return base + i;
        }

        return 0;
    }

    bool bytes_match(const uintptr_t address, const uint8_t* expected, const uint32_t length)
    {
        __try
        {
            return std::memcmp(reinterpret_cast<const void*>(address), expected, length) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool write_bytes_patch(BytesPatchSite& site, const uintptr_t address, const uint8_t* patch, const uint32_t length)
    {
        if (site.patched)
            return true;
        if (patch == nullptr || length == 0 || length > sizeof(site.original))
            return false;

        __try
        {
            std::memcpy(site.original, reinterpret_cast<const void*>(address), length);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        DWORD oldProtect = 0;
        if (!::VirtualProtect(reinterpret_cast<void*>(address), length, PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;

        std::memcpy(reinterpret_cast<void*>(address), patch, length);
        ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(address), length);

        DWORD unused = 0;
        ::VirtualProtect(reinterpret_cast<void*>(address), length, oldProtect, &unused);

        site.address = address;
        site.length = length;
        site.patched = true;
        return true;
    }

    bool write_jump_patch(BytesPatchSite& site, const uintptr_t address, void* destination, const uint32_t length)
    {
        if (site.patched)
            return true;
        if (destination == nullptr || length < 5 || length > sizeof(site.original))
            return false;

        __try
        {
            std::memcpy(site.original, reinterpret_cast<const void*>(address), length);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }

        DWORD oldProtect = 0;
        if (!::VirtualProtect(reinterpret_cast<void*>(address), length, PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;

        const intptr_t relative = reinterpret_cast<uintptr_t>(destination) - (address + 5);
        auto* out = reinterpret_cast<uint8_t*>(address);
        out[0] = 0xE9;
        std::memcpy(out + 1, &relative, sizeof(int32_t));
        for (uint32_t i = 5; i < length; ++i)
            out[i] = 0x90;

        ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(address), length);

        DWORD unused = 0;
        ::VirtualProtect(reinterpret_cast<void*>(address), length, oldProtect, &unused);

        site.address = address;
        site.length = length;
        site.patched = true;
        return true;
    }

    bool restore_patch(BytesPatchSite& site)
    {
        if (!site.patched)
            return true;

        DWORD oldProtect = 0;
        if (!::VirtualProtect(reinterpret_cast<void*>(site.address), site.length, PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;

        std::memcpy(reinterpret_cast<void*>(site.address), site.original, site.length);
        ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(site.address), site.length);

        DWORD unused = 0;
        ::VirtualProtect(reinterpret_cast<void*>(site.address), site.length, oldProtect, &unused);
        site = {};
        return true;
    }

    // Parse only the tokens needed for the conflict guard, without truncating commands.
    std::string_view next_command_token(std::string_view& rest)
    {
        while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front())))
            rest.remove_prefix(1);
        const bool quoted = !rest.empty() && rest.front() == '"';
        if (quoted) rest.remove_prefix(1);
        size_t length = 0;
        while (length < rest.size() && (quoted ? rest[length] != '"' :
            !std::isspace(static_cast<unsigned char>(rest[length])))) ++length;
        const auto token = rest.substr(0, length);
        rest.remove_prefix(length);
        if (quoted && !rest.empty()) rest.remove_prefix(1);
        return token;
    }

    bool command_token_equals(std::string_view token, std::string_view expected)
    {
        return token.size() == expected.size() && std::equal(token.begin(), token.end(),
            expected.begin(), [](unsigned char a, unsigned char b) {
                return std::tolower(a) == std::tolower(b);
            });
    }

    bool conflicts_with_drawdistance(const char* command)
    {
        if (!command) return false;
        std::string_view rest(command);
        const auto first = next_command_token(rest);
        if (command_token_equals(first, "/drawdistance")) return true;
        if (!command_token_equals(first, "/addon")) return false;
        const auto action = next_command_token(rest);
        if (!command_token_equals(action, "load") && !command_token_equals(action, "reload")) return false;
        auto name = next_command_token(rest);
        const auto slash = name.find_last_of("/\\");
        if (slash != std::string_view::npos) name.remove_prefix(slash + 1);
        return command_token_equals(name, "drawdistance") || command_token_equals(name, "drawdistance.lua");
    }

    bool iequals(const char* a, const char* b)
    {
        return _stricmp(a, b) == 0;
    }

    float clamp_float(float value, const float lo, const float hi)
    {
        if (value < lo)
            return lo;
        if (value > hi)
            return hi;
        return value;
    }

    constexpr float kDefaultDrawDistance = 1.0f;
    constexpr int kDistanceSliderMax = 40;
    constexpr float kMaxDrawDistance = 100.0f; // Original mapping preserves existing slider values.
    constexpr float kDrawDistanceSliderPower = 2.0f;
    constexpr float kDefaultDrawDistanceSlider = 20.0f;
    constexpr float kDefaultPropRange = 1.25f;
    constexpr float kLodRangeMin = 0.25f;
    constexpr float kLodRangeMax = 10.0f;
    constexpr int kConfigVersion = 9;
    constexpr const char* kZoneProfilesHelp =
        "Saves draw distance, object range, map LoD, fog and the three Terrain settings per zone. "
        "Zones without a saved profile use the global profile until you save settings for that zone.";
    constexpr const char* kDistanceHelp =
        "Adjusts scenery draw distance. In Distance fog mode, "
        "the game's fog range is still linked to this raised distance. Recommended: 20 or lower.";
    constexpr const char* kLodModeHelp =
        "Game uses native detail transitions. Full keeps high-detail models visible.";
    constexpr const char* kFogModeHelp =
        "Game uses the original fog distance for the current weather. Distance ties fog to your draw distance.";
    float g_lodRangeScaleSq = 1.0f;
    uintptr_t g_lodRangeReturn0 = 0;
    uintptr_t g_lodRangeReturn1 = 0;
    uintptr_t g_lodRangeReturn2 = 0;
    uintptr_t g_lodRangeReturn3 = 0;

    float g_propRangeScaleSquared = 1.0f;
    uintptr_t g_terrainRangeReturn = 0;
    constexpr uint32_t TerrainRangeStoreRva = 0x0017BF58;
    constexpr uint8_t TerrainRangeSetup[] = {
        0xD9, 0x9D, 0x24, 0x94, 0x03, 0x00, // fstp [ebp+39424h], the squared world radius
    };
    constexpr uint8_t TerrainRangeStore[] = {
        0xD9, 0x9D, 0x28, 0x94, 0x03, 0x00, // fstp [ebp+39428h], the per-block squared-range multiplier
    };
    constexpr uint8_t TerrainFarArguments[] = {
        0x8B, 0x85, 0xB4, 0x55, 0x03, 0x00, // mov eax, [ebp+355B4h]
        0x51, 0x68, 0xCD, 0xCC, 0x4C, 0x3E, // push far; push 0.2f
        0x51, 0x8D, 0xB5, 0x5C, 0x8E, 0x03, 0x00, // temporary fov; terrain projection destination
    };
    constexpr uint8_t TerrainFarProjectionCall[] = {
        0xD9, 0x1C, 0x24, 0x50, 0x51, 0x56, 0x8B, 0xCD, 0xE8, 0xD7, 0x64, 0x00, 0x00,
    };

    uintptr_t g_terrainGroupReturn = 0;
    // Lower 16 bits: Ashita zone ID. Bit 16: bypass permitted in this zone.
    volatile LONG g_terrainGroupPolicy = 0;
    constexpr uint32_t TerrainGroupSelectRva = 0x0017C2A8;
    constexpr uint32_t TerrainGroupHookLength = 6;
    constexpr uint8_t TerrainGroupSelect[] = {
        0x8B, 0x44, 0x24, 0x20, 0x3B, 0xC3, // mov eax, [esp+20h]; cmp eax, ebx
        0x0F, 0x84, 0xD1, 0x00, 0x00, 0x00, // Jump to the normal spatial traversal if no group is selected.
        0x39, 0x5E, 0xFC, 0x0F, 0x84, 0xC8, 0x00, 0x00, 0x00, // Same fallback if the map has no group table.
    };
    constexpr uint32_t TerrainGroupFallbackRva = 0x0017C385;
    constexpr uint8_t TerrainGroupFallback[] = {
        0x39, 0x1E, 0x74, 0x42, // cmp [esi], ebx; jump to full scan if quadtree is missing
        0x8B, 0x85, 0x4C, 0x94, 0x03, 0x00,
    };

    constexpr std::array<uint32_t, 4> LodRangeRvas{0x0017B629, 0x0017B641, 0x0017CA9E, 0x0017CABA};
    constexpr std::array<uint32_t, 2> LodHighRvas{0x0017B63A, 0x0017CAAF};
    constexpr uint8_t LodRangeExpected[][10] = {
        {0xD9, 0x44, 0x24, 0x14, 0xD8, 0x99, 0xCC, 0x00, 0x00, 0x00},
        {0xD9, 0x44, 0x24, 0x14, 0xD8, 0x99, 0xC8, 0x00, 0x00, 0x00},
        {0xD9, 0x44, 0x24, 0x58, 0xD8, 0x9F, 0xCC, 0x00, 0x00, 0x00},
        {0xD9, 0x44, 0x24, 0x58, 0xD8, 0x9F, 0xC8, 0x00, 0x00, 0x00},
    };
    constexpr uint8_t LodHighExpected[][2] = {{0x75, 0x05}, {0x75, 0x09}};
    constexpr uint8_t LodHighPatch[][2] = {{0xEB, 0x1D}, {0xEB, 0x25}};
    constexpr uint8_t LodHighTarget[][3] = {{0x8B, 0x59, 0x18}, {0x8B, 0x4F, 0x18}};

    struct TerrainSignature {
        uint32_t rva;
        const uint8_t* bytes;
        uint32_t length;
    };

    template<size_t N>
    uintptr_t terrain_site(uintptr_t base, uint32_t size, uint32_t siteRva,
        const TerrainSignature (&signatures)[N])
    {
        if (!base) return 0;
        for (const uint32_t shift : {0U, 0x15D0U}) {
            bool matches = true;
            for (const auto& sig : signatures) {
                const auto rva = sig.rva + shift;
                if (rva > size || sig.length > size - rva ||
                    !bytes_match(base + rva, sig.bytes, sig.length)) {
                    matches = false;
                    break;
                }
            }
            if (matches) return base + siteRva + shift;
        }
        return 0;
    }

    std::optional<uint32_t> lod_layout_shift(uintptr_t base, uint32_t size)
    {
        // The November retail layout is 0x15D0 bytes past the October layout.
        if (!base) return std::nullopt;
        for (const uint32_t shift : {0U, 0x1540U, 0x2B10U}) {
            const uint32_t lastByte = LodHighRvas.back() + shift + 2 + LodHighPatch[1][1] + sizeof(LodHighTarget[1]);
            if (lastByte > size) continue;
            bool matches = true;
            for (size_t i = 0; matches && i < LodRangeRvas.size(); ++i)
                matches = bytes_match(base + LodRangeRvas[i] + shift, LodRangeExpected[i], sizeof(LodRangeExpected[i]));
            for (size_t i = 0; matches && i < LodHighRvas.size(); ++i) {
                const uintptr_t site = base + LodHighRvas[i] + shift;
                matches = bytes_match(site, LodHighExpected[i], sizeof(LodHighExpected[i])) &&
                    bytes_match(site + 2 + LodHighPatch[i][1], LodHighTarget[i], sizeof(LodHighTarget[i]));
            }
            if (matches) return shift;
        }
        return std::nullopt;
    }

    float draw_distance_to_slider(const float value)
    {
        const float normalized = clamp_float((value - kDefaultDrawDistance) / (kMaxDrawDistance - kDefaultDrawDistance), 0.0f, 1.0f);
        return 1.0f + (std::pow(normalized, 1.0f / kDrawDistanceSliderPower) * 99.0f);
    }

    float slider_to_draw_distance(const float value)
    {
        const float normalized = clamp_float((clamp_float(std::round(value), 1.0f, kDistanceSliderMax) - 1.0f) / 99.0f, 0.0f, 1.0f);
        return kDefaultDrawDistance + (std::pow(normalized, kDrawDistanceSliderPower) * (kMaxDrawDistance - kDefaultDrawDistance));
    }

    float default_draw_distance()
    {
        return slider_to_draw_distance(kDefaultDrawDistanceSlider);
    }
}

extern "C" __declspec(naked) void vanaview_lod_range0_hook()
{
    __asm
    {
        fld dword ptr [esp + 14h]
        fdiv dword ptr [g_lodRangeScaleSq]
        fcomp dword ptr [ecx + 0CCh]
        jmp dword ptr [g_lodRangeReturn0]
    }
}

extern "C" __declspec(naked) void vanaview_lod_range1_hook()
{
    __asm
    {
        fld dword ptr [esp + 14h]
        fdiv dword ptr [g_lodRangeScaleSq]
        fcomp dword ptr [ecx + 0C8h]
        jmp dword ptr [g_lodRangeReturn1]
    }
}

extern "C" __declspec(naked) void vanaview_lod_range2_hook()
{
    __asm
    {
        fld dword ptr [esp + 58h]
        fdiv dword ptr [g_lodRangeScaleSq]
        fcomp dword ptr [edi + 0CCh]
        jmp dword ptr [g_lodRangeReturn2]
    }
}

extern "C" __declspec(naked) void vanaview_lod_range3_hook()
{
    __asm
    {
        fld dword ptr [esp + 58h]
        fdiv dword ptr [g_lodRangeScaleSq]
        fcomp dword ptr [edi + 0C8h]
        jmp dword ptr [g_lodRangeReturn3]
    }
}

// Scale the squared object distance before the client stores it.
extern "C" __declspec(naked) void vanaview_terrain_range_hook()
{
    __asm {
        fmul dword ptr [g_propRangeScaleSquared]
        fstp dword ptr [ebp + 0x39428]
        jmp dword ptr [g_terrainRangeReturn]
    }
}

extern "C" __declspec(naked) void vanaview_terrain_group_hook()
{
    __asm {
        mov eax, dword ptr [esp + 0x20]
        push edx
        mov edx, dword ptr [ebp + 0x3C] // Main zone ID, shared by its submaps.
        test edx, edx
        jz finish_group_selection
        cmp eax, ebx
        je finish_group_selection
        cmp dword ptr [esi - 4], ebx
        je finish_group_selection
        cmp dword ptr [esi], ebx // Quadtree for the current layout.
        je finish_group_selection // Preserve groups when the alternative is a full scan.
        or edx, 0x10000
        cmp edx, dword ptr [g_terrainGroupPolicy]
        jne finish_group_selection // Also covers a mismatch during a zone transition.
        mov eax, ebx // Satisfy the client's original branch to the quadtree.
        mov dword ptr [esp + 0x24], ebx // Consume the selection as the grouped path would.
    finish_group_selection:
        pop edx
        cmp eax, ebx
        jmp dword ptr [g_terrainGroupReturn]
    }
}

class VanaView final : public IPlugin
{
public:
    const char* GetName(void) const override { return "VanaView"; }
    const char* GetAuthor(void) const override { return "rockerudon and By-T0r"; }
    const char* GetDescription(void) const override { return "Tunes world draw distance, map LoD, fog, and ground texture filtering."; }
    const char* GetLink(void) const override { return ""; }
    double GetVersion(void) const override { return 2.13; }
    // The standard Addons plugin uses priority 0. Block conflicting commands before Lua runs.
    int32_t GetPriority(void) const override { return -1; }

    uint32_t GetFlags(void) const override
    {
        return static_cast<uint32_t>(Ashita::PluginFlags::UseCommands) |
            static_cast<uint32_t>(Ashita::PluginFlags::UseDirect3D);
    }

    bool Initialize(IAshitaCore* core, ILogManager*, const uint32_t) override
    {
        m_core = core;
        build_config_path();
        if (m_core && m_core->GetInstallPath()) {
            try {
                m_mipmaps.set_disk_directory(std::filesystem::path(m_core->GetInstallPath()) /
                    "config" / "plugins" / "VanaView-mips-v1");
            } catch (...) {
                chat("Mipmap disk cache unavailable. Auto can still generate mipmaps.");
            }
        }
        load_config();
        m_mipStartupPending = true;
        update_current_zone();
        m_scenery.clear();
        load_active_profile();
        refresh_ffximain();
        schedule_zone_distance();
        apply_lod_mode();
        apply_terrain_profile();
        set_prop_range(m_propRange);
        chat("VanaView %.2f loaded. Use /vv to configure.", GetVersion());
        return true;
    }

    void Release(void) override
    {
        m_scenery.clear();
        if (!m_shadows.reset(shadow_client())) chat("Could not restore native shadows; restart the client.");
        restore_patch(m_terrainRangePatch);
        ::InterlockedExchange(&g_terrainGroupPolicy, 0);
        restore_patch(m_terrainGroupPatch);
        ::InterlockedExchange(&FogDistanceScale::enabled, 0);
        restore_patch(m_fogScaleWorld);
        restore_patch(m_fogScaleEntity);
        m_mipmaps.stop();
        discard_ui_preview(false);
        save_config(false);
        restore_draw_distances();
        restore_lod_patches();
        m_fogMode = FogMode::DistanceLinked;
        m_device = nullptr;
    }

    bool Direct3DInitialize(IDirect3DDevice8* device) override
    {
        m_mipmaps.stop();
        m_scenery.clear();
        m_device = device;
        m_mipStartupPending = true;
        return true;
    }

    bool HandleCommand(int32_t, const char* command, bool) override
    {
        if (command == nullptr)
            return false;
        if (conflicts_with_drawdistance(command)) {
            chat("drawdistance is incompatible with VanaView. Use /vv for draw distance; /addon unload drawdistance to unload the addon.");
            return true;
        }

        char buffer[512]{};
        std::snprintf(buffer, sizeof(buffer), "%s", command);

        char* context = nullptr;
        char* token = strtok_s(buffer, " \t", &context);
        if (token == nullptr || (!iequals(token, "/vanaview") && !iequals(token, "/vv")))
            return false;

        const char* sub = strtok_s(nullptr, " \t", &context);
        if (sub == nullptr)
        {
            m_windowOpen = !m_windowOpen;
            if (!m_windowOpen) discard_ui_preview();
            return true;
        }

        const char* arg = strtok_s(nullptr, " \t", &context);
        const bool extra = strtok_s(nullptr, " \t", &context) != nullptr;
        if (iequals(sub, "status") && !arg) {
            print_status();
            return true;
        }
        if (iequals(sub, "coverage")) {
            int request = 0;
            if (!arg) request = 1;
            else if (iequals(arg,"on")) request = 2;
            else if (iequals(arg,"off")) request = 3;
            else if (iequals(arg,"status")) request = 4;
            if (!request || extra) chat("Usage: /vv coverage [on|off|status]");
            else {
                std::lock_guard lock(m_mipMutex);
                m_coverageRequests.push_back(request);
            }
            return true;
        }
        if (iequals(sub, "mip")) {
            if (extra) chat("Usage: /vv mip [on|off|status]");
            else if (!arg || iequals(arg,"status")) queue_mip_settings();
            else if (iequals(arg,"on") || iequals(arg,"off")) {
                if (draft_pending()) chat("Apply or close the settings preview before changing mipmaps.");
                else {
                    discard_ui_preview();
                    const bool enabled = iequals(arg,"on");
                    queue_mip_settings(enabled, enabled);
                }
            } else chat("Usage: /vv mip [on|off|status]");
            return true;
        }
        if (!iequals(sub,"help") || arg) chat("Unknown VanaView command or argument.");
        print_help();
        return true;
    }

    void Direct3DBeginScene(bool isRenderingBackBuffer) override
    {
        if (m_effectsEnabled && (m_shadows.simple || m_shadows.requested!=shadows::TerrainRange::Native || m_shadowUpdate)) {
            m_shadowUpdate=false;
            auto* client=shadow_client();
            if (!m_shadows.sync(client, GetModuleHandleW(L"NormalShadows.dll")!=nullptr)) {
                if (!m_shadows.reset(client)) chat("Could not restore native shadows; restart the client.");
                chat("Shadow override unavailable. Unload standalone NormalShadows and check the client version.");
            }
        }
        if (m_terrainGroupPatch.patched && isRenderingBackBuffer)
            update_terrain_group_zone(get_current_zone());

    }

    void Direct3DPresent(const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) override
    {
        UNREFERENCED_PARAMETER(pSourceRect);
        UNREFERENCED_PARAMETER(pDestRect);
        UNREFERENCED_PARAMETER(hDestWindowOverride);
        UNREFERENCED_PARAMETER(pDirtyRegion);

        const auto presentTime = FrameClock::now();
        const auto zone = get_current_zone();
        remember_frame(presentTime, zone);
        update_zone_profile(zone);
        if (m_zoneDistancePending) {
            const auto now=GetTickCount64();
            if (now>=m_zoneDistanceNextCheck)
                finish_zone_distance(local_player_ready(),now);
        }
        try {
            if (m_effectsEnabled) {
            m_mipmaps.present(m_device);
            if(m_mipmaps.mode()==TerrainMipmaps::Mode::Auto) m_scenery.present(m_device);
            }
        } catch (...) {
            m_mipmaps.stop();
            chat("Automatic mipmaps stopped: could not prepare the texture cache.");
        }
        process_mip_command();
        std::vector<int> requests;
        {
            std::lock_guard lock(m_mipMutex);
            requests.swap(m_coverageRequests);
        }
        for (int request : requests) {
            auto& coverage=m_mipmaps.coverage;
            if(request!=4) {
                bool enable=request==1?!coverage.enabled:request==2;
                D3DCAPS8 caps{};
                if(enable && (!m_device || FAILED(m_device->GetDeviceCaps(&caps)) ||
                   !(caps.RasterCaps&D3DPRASTERCAPS_FOGTABLE))) {
                    chat("Coverage marking unavailable: pixel fog unsupported.");
                    enable=false;
                }
                coverage.enabled=enable;coverage.marked=coverage.errors=0;
            }
            chat("Coverage %s: green = VanaView mip draw. Marked %u, errors %u.",
                 coverage.enabled?"on":"off",coverage.marked,coverage.errors);
        }
        render_window();
    }

    bool Direct3DDrawPrimitive(D3DPRIMITIVETYPE primitiveType, UINT startVertex, UINT primitiveCount) override
    {
        if (!m_effectsEnabled) return false;
        DWORD shader=0;
        if (m_device && m_scenery.groundEnabled && m_mipmaps.mode()==TerrainMipmaps::Mode::Auto &&
            SUCCEEDED(m_device->GetPixelShader(&shader))) {
            if (!shader && AutoColorMipmaps::uses_fixed_ground_cache(m_device,primitiveType))
                return m_mipmaps.draw(m_device,primitiveType,startVertex,primitiveCount);
            if (shader && m_mipmaps.draw(m_device,primitiveType,startVertex,primitiveCount))
                return true;
        }
        return draw_scenery(primitiveType,[&]{return m_device->DrawPrimitive(primitiveType,startVertex,primitiveCount);});
    }

    bool Direct3DDrawIndexedPrimitive(D3DPRIMITIVETYPE type, UINT minIndex, UINT vertices, UINT start, UINT count) override
    {
        return draw_scenery(type,[&]{return m_device->DrawIndexedPrimitive(type,minIndex,vertices,start,count);});
    }
    bool Direct3DDrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) override
    {
        return draw_scenery(type,[&]{return m_device->DrawPrimitiveUP(type,count,data,stride);});
    }
    bool Direct3DDrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT first, UINT vertices, UINT count,
        const void* indices, D3DFORMAT format, const void* data, UINT stride) override
    {
        return draw_scenery(type,[&]{return m_device->DrawIndexedPrimitiveUP(type,first,vertices,count,indices,format,data,stride);});
    }

private:
    std::vector<int> m_coverageRequests;
    SceneryMipmaps m_scenery;
    template<class Draw>
    bool draw_scenery(D3DPRIMITIVETYPE type,Draw submit) {
        if(!m_effectsEnabled || m_mipmaps.mode()!=TerrainMipmaps::Mode::Auto || !m_currentZoneId) {
            return false;
        }
        try {
            auto markedDraw=[&]{return m_mipmaps.coverage.submit(m_device,submit);};
            return m_scenery.draw(m_device,type,markedDraw);
        }
        catch(const std::bad_alloc&) { m_scenery.clear(); return false; }
    }
    static std::uint8_t* shadow_client() {
        return reinterpret_cast<std::uint8_t*>(GetModuleHandleW(L"FFXiMain.dll"));
    }
    shadows::NormalShadows m_shadows;
    bool m_shadowUpdate=false;
    bool m_effectsEnabled=true;
    bool set_effects_enabled(bool enabled) {
        if (enabled==m_effectsEnabled) return true;
        if (!enabled) {
            const bool simple=m_shadows.simple;
            const auto range=m_shadows.requested;
            const bool shadowsOk=m_shadows.reset(shadow_client());
            m_shadows.simple=simple; m_shadows.requested=range;
            const bool distanceOk=restore_draw_distances();
            const bool lodOk=restore_lod_patches();
            const bool rangeOk=restore_patch(m_terrainRangePatch);
            if (!shadowsOk || !distanceOk || !lodOk || !rangeOk) {
                apply_loaded_profile(); m_shadowUpdate=true;
                chat("Could not disable all VanaView effects. Comparison was cancelled.");
                return false;
            }
            m_effectsEnabled=false;
            ::InterlockedExchange(&FogDistanceScale::enabled,0);
            publish_terrain_group_policy();
            return true;
        }
        m_effectsEnabled=true;
        const bool terrainOk=!m_terrainGroupsEnabled || install_terrain_group_hook();
        const bool rangeOk=set_prop_range(m_propRange);
        const bool distanceOk=apply_draw_distances();
        const bool lodOk=apply_lod_mode();
        if (!terrainOk || !rangeOk || !distanceOk || !lodOk) {
            set_effects_enabled(false);
            chat("Could not enable all VanaView effects.");
            return false;
        }
        apply_active_fog_mode(); publish_terrain_group_policy();
        m_shadowUpdate=true; m_mipStartupPending=true;
        return true;
    }
    uint64_t m_effectsToggleReadyAt=0;
    unsigned comparison_wait_seconds(uint64_t now) const {
        return now>=m_effectsToggleReadyAt ? 0u : static_cast<unsigned>((m_effectsToggleReadyAt-now+999)/1000);
    }
    bool comparison_mipmaps_pending() const {
        return m_effectsEnabled && m_device && (m_scenery.groundEnabled || m_scenery.foliageEnabled) &&
            (m_mipStartupPending || (m_mipmaps.mode()==TerrainMipmaps::Mode::Auto &&
                (m_mipmaps.automatic().pending() || m_scenery.pending())));
    }
    void toggle_effects_from_ui(uint64_t now=GetTickCount64()) {
        if (comparison_wait_seconds(now) || comparison_mipmaps_pending()) return;
        if (set_effects_enabled(!m_effectsEnabled))
            m_effectsToggleReadyAt=now+3000;
    }
    void save_shadow_settings() {
        if (!write_config_value("shadows","simple",m_shadows.simple?"1":"0",m_configPath) ||
            !write_config_value("shadows","full_range",m_shadows.requested==shadows::TerrainRange::Full?"1":"0",m_configPath))
            chat("Could not save shadow settings.");
    }

    friend struct MipCommandTest;
    friend struct TerrainRoutingTest;
    struct MipRequest {
        std::optional<bool> ground, foliage;
        bool report=false;
    };
    void queue_mip_settings(std::optional<bool> ground={}, std::optional<bool> foliage={})
    {
        std::lock_guard lock(m_mipMutex);
        if (ground.has_value()) m_mipRequest.ground=ground;
        if (foliage.has_value()) m_mipRequest.foliage=foliage;
        m_mipRequest.report=true;
    }

    void start_ground_mipmaps()
    {
        try {
            std::string message;
            m_mipmaps.start_ground_mipmaps(m_device, message);
            if (!message.empty()) chat("%s", message.c_str());
        } catch (...) {
            m_mipmaps.stop();
            chat("Scenery mipmaps could not initialize.");
        }
    }

    void process_mip_command()
    {
        MipRequest request;
        {
            std::lock_guard lock(m_mipMutex);
            request=std::exchange(m_mipRequest, MipRequest{});
        }
        if (request.ground || request.foliage) {
            if (request.ground) m_scenery.groundEnabled=*request.ground;
            if (request.foliage) m_scenery.foliageEnabled=*request.foliage;
            save_config();
            m_mipStartupPending=true;
        }
        if (m_effectsEnabled && m_device && std::exchange(m_mipStartupPending, false)) {
            if (m_scenery.groundEnabled || m_scenery.foliageEnabled) {
                if (m_mipmaps.mode() != TerrainMipmaps::Mode::Auto) start_ground_mipmaps();
            } else {
                m_mipmaps.stop();
                m_scenery.clear();
            }
        }
        if (!request.report) return;
        chat("Scenery mipmaps %s (ground %s, foliage %s): %llu chains, %llu scenery draws (%llu alpha-tested), %llu terrain draws, %llu errors.",
            m_mipmaps.mode()==TerrainMipmaps::Mode::Auto?"active":"inactive",
            m_scenery.groundEnabled?"on":"off",m_scenery.foliageEnabled?"on":"off",
            m_scenery.generated,m_scenery.draws,m_scenery.cutouts,m_mipmaps.stats().color,m_scenery.failed);
    }

    enum class FogMode : int
    {
        DistanceLinked = 0,
        Game = 2,
    };

    void print_help()
    {
        chat("/vv or /vanaview - open or close settings");
        chat("/vv help | /vv status - list commands or show draw distance, map LoD and fog");
        chat("/vv mip [on|off|status] - switch both mipmap settings or show status");
        chat("/vv coverage [on|off|status] - toggle or inspect the green mipmap overlay");
    }

    void print_status()
    {
        chat(
            "VanaView: distance=%.0f lod=%s %.2fx ptr=%s",
            static_cast<double>(draw_distance_to_slider(m_drawDistance)),
            lod_mode_name(),
            static_cast<double>(m_lodRangeScale),
            (m_worldDistanceAddress != 0 && m_entityDistanceAddress != 0) ? "ok" : "missing");
        print_fog_status();
    }

    const char* lod_mode_name() const
    {
        if (m_lodMode == LodMode::High)
            return "full";
        if (m_lodMode == LodMode::Range)
            return "range";
        return "default";
    }

    FogMode parse_fog_mode(const int value) const
    {
        // Legacy Custom (1) profiles now use the native weather calculation.
        return value == 0 ? FogMode::DistanceLinked : FogMode::Game;
    }

    void apply_active_fog_mode()
    {
        ::InterlockedExchange(&FogDistanceScale::enabled, 0);
        if (m_effectsEnabled && m_fogMode == FogMode::Game) {
            if (install_native_fog_scale())
                ::InterlockedExchange(&FogDistanceScale::enabled, 1);
            else
                chat("Native fog unavailable for this client. Fog follows draw distance.");
        }
    }

    void show_help_marker(IGuiManager* gui, const char* text)
    {
        if (gui == nullptr || text == nullptr)
            return;

        gui->SameLine(0.0f, 4.0f);
        gui->TextDisabled("(?)");
        if (!gui->IsItemHovered())
            return;

        gui->BeginTooltip();
        gui->PushTextWrapPos(gui->GetFontSize() * 35.0f);
        gui->TextWrapped("%s", text);
        gui->PopTextWrapPos();
        gui->EndTooltip();
    }

    int push_theme(IGuiManager* gui)
    {
        if (gui == nullptr)
            return 0;

        int count = 0;
        auto push = [&](const ImGuiCol idx, const float r, const float g, const float b, const float a)
        {
            gui->PushStyleColor(idx, ImVec4(r, g, b, a));
            ++count;
        };

        push(ImGuiCol_WindowBg, 0.10f, 0.10f, 0.10f, 0.94f);
        push(ImGuiCol_ChildBg, 0.12f, 0.12f, 0.12f, 0.60f);
        push(ImGuiCol_PopupBg, 0.10f, 0.10f, 0.10f, 0.96f);
        push(ImGuiCol_Border, 0.30f, 0.30f, 0.30f, 0.50f);
        push(ImGuiCol_FrameBg, 0.18f, 0.18f, 0.18f, 0.82f);
        push(ImGuiCol_FrameBgHovered, 0.28f, 0.28f, 0.28f, 0.88f);
        push(ImGuiCol_FrameBgActive, 0.36f, 0.36f, 0.36f, 0.95f);
        push(ImGuiCol_TitleBg, 0.08f, 0.08f, 0.08f, 1.00f);
        push(ImGuiCol_TitleBgActive, 0.14f, 0.14f, 0.14f, 1.00f);
        push(ImGuiCol_Button, 0.24f, 0.24f, 0.24f, 0.82f);
        push(ImGuiCol_ButtonHovered, 0.34f, 0.34f, 0.34f, 0.90f);
        push(ImGuiCol_ButtonActive, 0.44f, 0.44f, 0.44f, 0.96f);
        push(ImGuiCol_Header, 0.24f, 0.24f, 0.24f, 0.82f);
        push(ImGuiCol_HeaderHovered, 0.34f, 0.34f, 0.34f, 0.90f);
        push(ImGuiCol_HeaderActive, 0.44f, 0.44f, 0.44f, 0.96f);
        push(ImGuiCol_ScrollbarBg, 0.05f, 0.05f, 0.05f, 0.35f);
        push(ImGuiCol_ScrollbarGrab, 0.42f, 0.42f, 0.42f, 0.72f);
        push(ImGuiCol_ScrollbarGrabHovered, 0.56f, 0.56f, 0.56f, 0.86f);
        push(ImGuiCol_ScrollbarGrabActive, 0.70f, 0.70f, 0.70f, 0.96f);
        push(ImGuiCol_CheckMark, 0.82f, 0.82f, 0.82f, 1.00f);
        push(ImGuiCol_SliderGrab, 0.58f, 0.58f, 0.58f, 0.92f);
        push(ImGuiCol_SliderGrabActive, 0.78f, 0.78f, 0.78f, 1.00f);
        push(ImGuiCol_Tab, 0.18f, 0.18f, 0.18f, 0.86f);
        push(ImGuiCol_TabHovered, 0.36f, 0.36f, 0.36f, 0.90f);
        push(ImGuiCol_TabActive, 0.30f, 0.30f, 0.30f, 1.00f);
        push(ImGuiCol_TextSelectedBg, 0.55f, 0.55f, 0.55f, 0.45f);
        return count;
    }

    enum class LodMode : int
    {
        Off = 0,
        High = 1,
        Range = 2,
    };


    struct SettingsDraft {
        float distance, range, lodRange;
        LodMode lod;
        FogMode fog;
        bool zones, terrain, ground, foliage, simple, excluded;
        shadows::TerrainRange shadowRange;
        bool operator==(const SettingsDraft&) const = default;
    };
    std::optional<SettingsDraft> m_uiDraft;
    std::optional<SettingsDraft> m_uiSaved;
    bool m_technicalDescriptions=true;
    bool m_uiSaveFailed=false;
    bool m_resetAllPending=false;
    bool m_configWriteFailed=false;
    bool write_config_value(const char* section,const char* key,const char* value,const char* path) {
        const bool ok=::WritePrivateProfileStringA(section,key,value,path)!=0;
        m_configWriteFailed |= !ok;
        return ok;
    }
    bool set_technical_descriptions(bool enabled) {
        // Presentation preferences save independently; never apply a graphics draft.
        if (!::WritePrivateProfileStringA("interface", "technical_descriptions", enabled ? "1" : "0", m_configPath)) {
            chat("Could not save the interface preference. Check config write access.");
            return false;
        }
        m_technicalDescriptions=enabled;
        return true;
    }
    SettingsDraft current_settings() const {
        return {m_drawDistance,m_propRange,m_lodRangeScale,m_lodMode,m_fogMode,m_useZoneProfiles,
            m_terrainGroupsEnabled,m_scenery.groundEnabled,m_scenery.foliageEnabled,m_shadows.simple,
            m_terrainGroupZoneExcluded,m_shadows.requested};
    }
    void assign_settings(const SettingsDraft& d) {
        m_drawDistance=d.distance; m_propRange=d.range; m_lodRangeScale=d.lodRange;
        m_lodMode=d.lod; m_fogMode=d.fog; m_useZoneProfiles=d.zones;
        m_terrainGroupsEnabled=d.terrain; m_scenery.groundEnabled=d.ground;
        m_scenery.foliageEnabled=d.foliage; m_shadows.simple=d.simple;
        m_terrainGroupZoneExcluded=d.excluded; m_shadows.requested=d.shadowRange;
    }
    SettingsDraft profile_draft(bool zones, bool globalOnly=false) {
        const auto saved=current_settings();
        load_global_profile(kConfigVersion);
        if(zones && !globalOnly && m_currentZoneId) load_zone_profile(m_currentZoneId);
        auto result=current_settings(); result.zones=zones;
        assign_settings(saved);
        return result;
    }
    bool draft_pending() const { return m_uiDraft && (m_resetAllPending || m_uiSaveFailed || !(*m_uiDraft==m_uiSaved.value_or(current_settings()))); }
    bool preview_ui_draft() {
        if (!m_uiDraft || *m_uiDraft==current_settings()) return true;
        const auto before=current_settings();
        const auto desired=*m_uiDraft;
        if (desired.terrain && !install_terrain_group_hook()) return false;
        if (!set_prop_range(desired.range)) return false;
        if (!m_uiSaved) m_uiSaved=before;
        assign_settings(desired);
        apply_loaded_profile();
        m_terrainGroupZoneExcluded=desired.excluded;
        publish_terrain_group_policy();
        m_shadowUpdate=true;
        return true;
    }
    void discard_ui_preview(bool updateRuntime=true) {
        if (m_uiSaved) {
            const auto saved=*m_uiSaved;
            assign_settings(saved);
            if (updateRuntime) {
                apply_loaded_profile();
                m_terrainGroupZoneExcluded=saved.excluded;
                publish_terrain_group_policy();
                m_shadowUpdate=true;
            }
        }
        m_uiSaved.reset(); m_uiDraft.reset(); m_uiSaveFailed=false; m_resetAllPending=false;
    }
    SettingsDraft default_graphics_settings(bool excluded) const {
        return SettingsDraft{default_draw_distance(),kDefaultPropRange,1.0f,LodMode::High,FogMode::Game,
            false,true,true,true,shadows::NormalShadows::defaultSimple,excluded,shadows::NormalShadows::defaultRange};
    }
    bool stage_factory_defaults() {
        m_uiDraft=default_graphics_settings(false);
        m_resetAllPending=preview_ui_draft();
        if (!m_resetAllPending) m_uiDraft=current_settings();
        return m_resetAllPending;
    }

    bool save_factory_reset() {
        // Publish one complete INI so every zone is reset together, including unvisited zones.
        char original[MAX_PATH]{}, directory[MAX_PATH]{}, temporary[MAX_PATH]{};
        strcpy_s(original,m_configPath);
        strcpy_s(directory,original);
        char* slash=std::strrchr(directory,'\\');
        if (!slash) return false;
        slash[1]='\0';
        if (!::GetTempFileNameA(directory,"vvr",0,temporary)) return false;
        strcpy_s(m_configPath,temporary);
        save_shadow_settings();
        save_config();
        ::WritePrivateProfileStringA(nullptr,nullptr,nullptr,temporary);
        strcpy_s(m_configPath,original);
        if (m_configWriteFailed || !::MoveFileExA(temporary,original,MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            ::DeleteFileA(temporary);
            return false;
        }
        m_activeZoneHasProfile=false;
        m_technicalDescriptions=true;
        return true;
    }

    bool apply_ui_draft() {
        if (!draft_pending()) return true;
        if (!preview_ui_draft()) return false;
        const auto d=*m_uiDraft;
        m_configWriteFailed=false;
        m_uiSaveFailed=true;
        if (m_resetAllPending) {
            if (!save_factory_reset()) { chat("Could not save the reset. Check config write access and retry Apply."); return false; }
            m_resetAllPending=false;
        } else {
            save_shadow_settings();
            if (m_currentZoneId) {
                char key[8]{}; std::snprintf(key,sizeof(key),"%u",unsigned(m_currentZoneId));
                if (!write_config_value("terrain_groups_excluded",key,d.excluded?"1":nullptr,m_configPath)) {
                    chat("Could not save the terrain exclusion."); return false;
                }
            }
            save_config();
            if (m_configWriteFailed) { chat("Could not save all settings. Check write access and retry Apply."); return false; }
        }
        m_uiSaveFailed=false;
        m_uiDraft=current_settings();
        m_uiSaved.reset();
        return true;
    }

    void render_window()
    {
        if (!m_windowOpen || m_core == nullptr || m_core->GetGuiManager() == nullptr)
            return;

        auto* gui = m_core->GetGuiManager();
        const int themeCount = push_theme(gui);
        const float scale = clamp_float(gui->GetIO().DisplaySize.y / 1080.0f, 1.0f, 2.0f);
        const float bodyFont = gui->GetStyle().FontSizeBase * scale * 0.90f;
        gui->PushFont(nullptr, bodyFont);
        gui->PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * scale, 10.0f * scale));
        gui->PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * scale, 3.0f * scale));
        gui->PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f * scale, 2.0f * scale));
        gui->PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * scale);
        gui->SetNextWindowSize(ImVec2(480.0f * scale,
            std::min(645.0f * scale + 2.0f * gui->GetTextLineHeightWithSpacing(), std::max(240.0f, gui->GetIO().DisplaySize.y - 40.0f * scale))), ImGuiCond_Always);
        const auto section = [&](const char* title) {
            gui->Spacing();
            gui->PushFont(nullptr, bodyFont * 1.08f);
            gui->PushStyleColor(ImGuiCol_Text, ImVec4(0.68f, 0.83f, 0.92f, 1.0f));
            gui->SeparatorText(title);
            gui->PopStyleColor();
            gui->PopFont();
        };
        const auto recommendation = [&](const char* text, bool exceeded) {
            gui->PushFont(nullptr, bodyFont * 0.88f);
            gui->PushStyleColor(ImGuiCol_Text, exceeded ? ImVec4(1.0f, 0.68f, 0.30f, 1.0f) : ImVec4(0.68f, 0.70f, 0.73f, 1.0f));
            gui->TextWrapped("%s", text);
            gui->PopStyleColor();
            gui->PopFont();
        };

        const auto wording = [&](const char* plain, const char* technical) {
            return m_technicalDescriptions ? technical : plain;
        };
        bool open = m_windowOpen;
        if (gui->Begin("VanaView", &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        {
            if (!m_uiDraft) { m_uiDraft=current_settings(); m_uiSaveFailed=false; }
            auto& d=*m_uiDraft;
            gui->BeginChild("settingsContent", ImVec2(0.0f, -95.0f * scale));
            gui->BeginDisabled(!m_effectsEnabled);
            bool profiles = d.zones;
            if (gui->Checkbox(wording("Different settings for each area###zoneProfiles", "Use zone profiles###zoneProfiles"), &profiles)) {
                m_resetAllPending=false;
                d=profile_draft(profiles);
            }
            show_help_marker(gui, wording("Turn this on to tune each area separately. Areas you have not customized use your shared settings. Shadows always apply everywhere. Click Apply to save.", kZoneProfilesHelp));
            if (d.zones && m_currentZoneId)
                gui->TextDisabled(wording("Area %u - %s", "Zone %u - %s"), unsigned(m_currentZoneId),
                    draft_pending() ? "pending changes" : (m_activeZoneHasProfile ? "custom settings" : "using shared settings"));
            else
                gui->TextDisabled("Shared settings for all areas");
            show_help_marker(gui, "Changes preview immediately. Apply saves the profile. Closing the window or changing zone/profile restores saved settings.");
            gui->TextWrapped("%s", d.zones
                ? "To edit global settings, turn off the checkbox above. Your zone profiles are kept."
                : "Edit global settings and click Apply. To resume zone profiles, turn them back on and click Apply again.");
            if (d.zones && m_currentZoneId) {
                gui->BeginDisabled(!m_activeZoneHasProfile && !draft_pending());
                if (gui->Button(wording("Use shared settings here###globalZone", "Use global defaults for this zone###globalZone")))
                    d=profile_draft(true,true);
                gui->EndDisabled();
                show_help_marker(gui, "Click Apply to save the shared settings in this area again. Other areas keep their own settings.");
            } else if (!d.zones) {
                gui->BeginDisabled(m_resetAllPending);
                if (gui->Button("Reset global settings"))
                    d=default_graphics_settings(d.excluded);
                gui->EndDisabled();
                show_help_marker(gui, "Preview factory defaults for global graphics settings, including shadows. Click Apply to save. Saved zone profiles and terrain exclusions are kept.");
            }
            section(wording("See more of the world", "Visibility"));
            int distance = static_cast<int>(std::lround(draw_distance_to_slider(d.distance)));
            gui->TextUnformatted(wording("See farther", "Draw distance"));
            show_help_marker(gui, wording("Adjusts how far you can see the scenery. Keep this at 20 or lower; higher values can cause low FPS.", kDistanceHelp));
            gui->SetNextItemWidth(-1.0f);
            if (gui->SliderInt("##drawDistance", &distance, 1, kDistanceSliderMax))
            {
                d.distance = slider_to_draw_distance(distance);
            }

            recommendation(distance > 20 ? "Above 20 is not recommended (low FPS)." : "Recommended: 20 or lower.", distance > 20);
            gui->Spacing();
            int range = static_cast<int>(std::lround(d.range * 4));
            char rangeLabel[24];
            std::snprintf(rangeLabel, sizeof(rangeLabel), "%.2fx", double(d.range));
            gui->TextUnformatted(wording("See scenery farther away", "Object range"));
            show_help_marker(gui, wording("Extends the range of map objects such as trees, rocks and flowers. Try 1.25x or 1.50x first. This is separate from the overall view distance above.", "Scales map-object distance independently of the overall draw distance. Range: 1x to 4x in 0.25x steps."));
            gui->SetNextItemWidth(-1.0f);
            if (gui->SliderInt("##objectRange", &range, 4, 16, rangeLabel, ImGuiSliderFlags_NoInput))
                d.range=range * 0.25f;
            recommendation(d.range > 1.5f ? "Above 1.50x is not recommended (low FPS)." : "Recommended: 1.50x or lower.", d.range > 1.5f);

            gui->Spacing();
            gui->Text(wording("Scenery detail", "Map LoD"));
            show_help_marker(gui, wording("Keep detail uses the most detailed scenery models available, reducing shape changes as you approach. It does not stop every object from appearing suddenly. Game default lets the game switch detail with distance.", kLodModeHelp));

            int lodMode = static_cast<int>(d.lod);
            bool lodChanged = false;
            lodChanged = gui->RadioButton(wording("Game default###lodGame", "Game###lodGame"), &lodMode, static_cast<int>(LodMode::Off));
            gui->SameLine();
            lodChanged = gui->RadioButton(wording("Keep detail###lodFull", "Full###lodFull"), &lodMode, static_cast<int>(LodMode::High)) || lodChanged;
            if (lodChanged)
            {
                d.lod = static_cast<LodMode>(lodMode);
            }

            gui->Spacing();
            gui->Text("Fog");
            show_help_marker(gui, wording("Weather fog keeps the usual mist for the current weather, even with a longer view. Extend with view moves the fog farther out as you increase view distance. It does not turn fog off.", kFogModeHelp));

            if (gui->RadioButton(wording("Weather fog###fogGame", "Game###fogGame"), d.fog == FogMode::Game)) {
                d.fog=FogMode::Game;
            }
            gui->SameLine();
            if (gui->RadioButton(wording("Extend with view###fogDistance", "Distance###fogDistance"), d.fog == FogMode::DistanceLinked)) {
                d.fog=FogMode::DistanceLinked;
            }

            section(wording("Steadier scenery", "Terrain"));
            const bool editingZone = d.zones && m_currentZoneId;
            bool extensionEnabled = d.terrain && !(editingZone && d.excluded);
            if (gui->Checkbox("Enable extension###terrain", &extensionEnabled)) {
                d.terrain = extensionEnabled;
                if (editingZone && extensionEnabled)
                    d.excluded = false;
            }
            show_help_marker(gui, editingZone
                ? "Keeps distant terrain visible in this zone on supported maps. Click Apply to save."
                : "Keeps distant terrain visible on supported maps. Zones with custom terrain settings keep their settings.");
            gui->Checkbox(wording("Reduce ground shimmer###ground", "Ground mipmaps###ground"), &d.ground);
            show_help_marker(gui, wording("Helps ground and other solid scenery look steadier while the camera moves. Keep it on if distant textures flicker.", "Generates mipmaps for supported terrain and opaque scenery textures."));
            gui->Checkbox(wording("Reduce shimmer on grass and leaves###foliage", "Foliage mipmaps###foliage"), &d.foliage);
            show_help_marker(gui, wording("Helps grass, leaves and flowers look steadier while the camera moves. Keep it on to reduce flicker.", "Reduces shimmer on grass, leaves and flowers."));

            section(wording("Shadows (everywhere)", "Shadows (all zones)"));
            bool shadowsChanged=gui->Checkbox(wording("Add simple character shadows###simpleShadows", "Simple actor shadows in Normal###simpleShadows"), &d.simple);
            show_help_marker(gui,wording("Adds simple ground shadows, including under your own character. Requires Shadows set to Normal in the game. High keeps detailed shadows; Off stays off.", "Uses simple actor shadows in the game's Normal mode. High and Off follow the game."));
            gui->Text(wording("Shadows on distant slopes", "Terrain shadow range"));
            show_help_marker(gui,wording("In the game's Normal shadow mode, Follow terrain keeps ground shadows following slopes farther away. Game default uses the original limited range. This does not create detailed character silhouettes.", "Native follows the terrain up to 20 units. Full follows it at all visible distances in Normal mode."));
            int shadowRange=static_cast<int>(d.shadowRange);
            shadowsChanged=gui->RadioButton(wording("Game default###shadowNative", "Native###shadowNative"),&shadowRange,static_cast<int>(shadows::TerrainRange::Native)) || shadowsChanged;
            gui->SameLine();
            shadowsChanged=gui->RadioButton(wording("Follow terrain###shadowFull", "Full###shadowFull"),&shadowRange,static_cast<int>(shadows::TerrainRange::Full)) || shadowsChanged;
            if (shadowsChanged) {
                d.shadowRange=static_cast<shadows::TerrainRange>(shadowRange);
            }
            gui->EndDisabled();
            if (!preview_ui_draft()) {
                chat("Could not preview these settings.");
                d=current_settings();
            }
            gui->EndChild();
            gui->Separator();
            const auto compareNow=GetTickCount64();
            const unsigned waitSeconds=comparison_wait_seconds(compareNow);
            const bool loadingMipmaps=comparison_mipmaps_pending();
            char compareLabel[64]{};
            if (waitSeconds) std::snprintf(compareLabel,sizeof(compareLabel),"Wait %us###compareEffects",waitSeconds);
            else std::snprintf(compareLabel,sizeof(compareLabel),"%s###compareEffects",loadingMipmaps ? "Loading mipmaps..." : (m_effectsEnabled ? "Disable effects" : "Enable effects"));
            gui->BeginDisabled(waitSeconds || loadingMipmaps);
            if (gui->Button(compareLabel, ImVec2(180.0f * scale,0.0f)))
                toggle_effects_from_ui(compareNow);
            gui->EndDisabled();
            gui->SameLine();
            gui->TextDisabled(m_effectsEnabled ? "VanaView: ON" : "VanaView: OFF");
            show_help_marker(gui,"Compare with VanaView effects disabled. Your profile and unsaved settings are kept. Other plugins and graphics wrappers are unaffected. Wait at least 3 seconds between clicks. When effects are on, the button also waits for queued mipmaps. Keep the camera still to compare. This comparison choice is not saved.");
            gui->PushFont(nullptr, bodyFont * 0.90f);
            bool friendly=!m_technicalDescriptions;
            if (gui->Checkbox("Friendly descriptions", &friendly))
                set_technical_descriptions(!friendly);
            show_help_marker(gui, "Uses plain language for setting names and explanations. Saved immediately for the next load, separately from graphics settings.");
            const float guideWidth = gui->CalcTextSize("Quick guide").x + 2.0f * gui->GetStyle().FramePadding.x;
            gui->SameLine(gui->GetCursorPosX() + gui->GetContentRegionAvail().x - guideWidth);
            if (gui->Button("Quick guide")) gui->OpenPopup("VanaView quick guide");
            gui->PopFont();
            recommendation(m_resetAllPending ? "Apply deletes all saved zone profiles." : draft_pending() ? "Unsaved changes - click Apply." : "No pending changes.", draft_pending());
            const auto& style = gui->GetStyle();
            const auto buttonWidth = [&](const char* label) {
                return gui->CalcTextSize(label).x + 2.0f * style.FramePadding.x;
            };
            const float available = gui->GetContentRegionAvail().x;
            const float left = gui->GetCursorPosX();
            const float secondaryWidth = std::max(buttonWidth("Diagnostic report"), buttonWidth("Reset all settings"));
            const float minimumSaveWidth = std::max(90.0f * scale, buttonWidth("Apply"));
            const bool singleRow = 2.0f * secondaryWidth + 2.0f * style.ItemSpacing.x + minimumSaveWidth <= available;
            const float saveWidth = singleRow ? available - 2.0f * secondaryWidth - 2.0f * style.ItemSpacing.x : minimumSaveWidth;
            const float settingsWidth = secondaryWidth + style.ItemSpacing.x + saveWidth;
            const auto reportTime = FrameClock::now();
            const bool reportSaved = reportTime < m_nextReportTime;
            const float reportWidth = secondaryWidth;
            gui->BeginDisabled(reportSaved);
            const bool saveReport = gui->Button(reportSaved ? "Report saved###diagnosticReport" :
                "Diagnostic report###diagnosticReport", ImVec2(reportWidth, 0.0f));
            gui->EndDisabled();
            if (saveReport) {
                try {
                    const auto install = m_core->GetInstallPath();
                    if (!install || !*install) throw std::runtime_error("Missing install directory");
                    const auto path = save_diagnostic_report(std::filesystem::path(install) /
                        "logs" / "VanaView", reportTime);
                    if (!path.empty()) chat("Report saved: logs/VanaView/%s", path.filename().string().c_str());
                } catch (const std::exception&) {
                    chat("Could not save the diagnostic report. Check write access to logs/VanaView and free disk space.");
                }
            }
            if (singleRow)
                gui->SameLine(0.0f, style.ItemSpacing.x);
            else if (settingsWidth <= available)
                gui->SetCursorPosX(left + available - settingsWidth);
            constexpr auto resetDialog = "Reset global settings and all zones?";
            gui->BeginDisabled(!m_effectsEnabled);
            if (gui->Button("Reset all settings", ImVec2(secondaryWidth, 0.0f)))
                gui->OpenPopup(resetDialog);

            if (settingsWidth <= available) gui->SameLine();
            gui->PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.36f, 0.46f, 1.0f));
            gui->PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.21f, 0.46f, 0.57f, 1.0f));
            gui->PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.13f, 0.29f, 0.38f, 1.0f));
            gui->BeginDisabled(!draft_pending());
            if (gui->Button("Apply", ImVec2(saveWidth, 0.0f))) {
                if (apply_ui_draft()) chat("Profile saved.");
            }
            gui->EndDisabled();
            gui->PopStyleColor(3);
            gui->EndDisabled();

            gui->SetNextWindowSize(ImVec2(440.0f * scale, 0.0f), ImGuiCond_Always);
            gui->SetNextWindowSizeConstraints(ImVec2(440.0f * scale, 0.0f),
                ImVec2(440.0f * scale, gui->GetIO().DisplaySize.y - 40.0f * scale));
            if (gui->BeginPopupModal("VanaView quick guide", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                gui->TextWrapped("Change one option and move the camera to compare immediately. Click Apply to save the profile. Closing without saving restores the saved settings.");
                section("See farther");
                gui->TextWrapped("%s: overall scenery distance. Recommended: 20 or lower.\n\n%s: trees, rocks and flowers. Try 1.25x or 1.50x.",
                    wording("See farther", "Draw distance"), wording("See scenery farther away", "Object range"));
                section("Steadier scenery");
                gui->TextWrapped("Changing shapes as you approach? Choose %s.\n\nFlickering textures? Enable these two options:\n- %s\n- %s",
                    wording("Keep detail", "Full under Map LoD"), wording("Reduce ground shimmer", "Ground mipmaps"),
                    wording("Reduce shimmer on grass and leaves", "Foliage mipmaps"));
                section("Weather and performance");
                gui->TextWrapped("Keep %s for the usual atmosphere.\n\nLow FPS? Lower the two distance sliders.",
                    wording("Weather fog", "Game under Fog"));
                gui->Spacing();
                if (gui->Button("Close", ImVec2(80.0f * scale, 0.0f)) || gui->IsKeyPressed(ImGuiKey_Escape, false))
                    gui->CloseCurrentPopup();
                gui->EndPopup();
            }

            if (gui->BeginPopupModal(resetDialog, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                gui->TextUnformatted("Restore global settings and descriptions to their defaults?");
                gui->TextUnformatted("Apply will delete every saved zone profile and terrain exclusion.");
                gui->Spacing();
                const bool cancel = gui->Button("Cancel") || gui->IsKeyPressed(ImGuiKey_Escape, false);
                gui->SetItemDefaultFocus();
                gui->SameLine();
                if (gui->Button("Reset all settings") && !cancel) {
                    if (!stage_factory_defaults()) chat("Could not preview factory defaults.");
                    gui->CloseCurrentPopup();
                } else if (cancel) {
                    gui->CloseCurrentPopup();
                }
                gui->EndPopup();
            }
        }
        gui->End();
        gui->PopStyleVar(4);
        gui->PopFont();

        m_windowOpen = open;
        if (!open) discard_ui_preview();
        if (themeCount > 0)
            gui->PopStyleColor(themeCount);
    }

    using FrameClock = std::chrono::steady_clock;
    struct FrameSample { double ms; uint16_t zone; };
    std::array<FrameSample, 900> m_recentFrames{};
    size_t m_frameNext = 0, m_frameCount = 0;
    FrameClock::time_point m_previousPresent{};
    unsigned m_reportNumber = 0;
    FrameClock::time_point m_nextReportTime = FrameClock::time_point::min();

    void remember_frame(FrameClock::time_point now, uint16_t zone)
    {
        if (m_previousPresent != FrameClock::time_point{}) {
            m_recentFrames[m_frameNext] = {
                std::chrono::duration<double, std::milli>(now - m_previousPresent).count(), zone};
            m_frameNext = (m_frameNext + 1) % m_recentFrames.size();
            m_frameCount = std::min(m_frameCount + 1, m_recentFrames.size());
        }
        m_previousPresent = now;
    }

    std::filesystem::path save_diagnostic_report(const std::filesystem::path& directory, FrameClock::time_point requestedAt)
    {
        if (requestedAt < m_nextReportTime) return {};
        SYSTEMTIME t{};
        GetLocalTime(&t);
        char name[96]{};
        std::snprintf(name, sizeof(name), "VanaView-%04u%02u%02u-%02u%02u%02u-%lu-%u.txt",
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
            GetCurrentProcessId(), ++m_reportNumber);
        std::filesystem::create_directories(directory);
        const auto path = directory / name;
        std::ofstream out;
        out.exceptions(std::ios::badbit | std::ios::failbit);
        out.open(path);
        try {
            out << "VanaView " << std::fixed << std::setprecision(2) << GetVersion()
                << " diagnostic report\nZone: " << m_currentZoneId
                << "\nDraw distance: " << std::lround(draw_distance_to_slider(m_drawDistance)) << "  Object range: " << m_propRange
                << "\nFog: " << (m_fogMode == FogMode::Game ? "Game" : "Distance")
                << "  Simple shadows: " << (m_shadows.simple ? "on" : "off")
                << "  Terrain shadow range: " << (m_shadows.requested == shadows::TerrainRange::Full ? "Full" : "Native")
                << "\nVanaView effects: " << (m_effectsEnabled ? "on" : "off (comparison)")
                << "\nGround mipmaps: " << (m_scenery.groundEnabled ? "on" : "off")
                << "  Foliage mipmaps: " << (m_scenery.foliageEnabled ? "on" : "off")
                << "  Coverage overlay: " << (m_mipmaps.coverage.enabled ? "on" : "off") << '\n';
            const auto& cache = m_mipmaps.automatic();
            const auto& stats = cache.stats();
            out << "Ground textures ready: " << cache.count() << "  Queued: " << cache.waiting()
                << "  Preparing: " << cache.preparing() << "  Uploading: " << cache.uploading()
                << "\nGround disk cache hits/misses/writes/errors: " << stats.diskHits << '/'
                << stats.diskMisses << '/' << stats.diskWrites << '/' << stats.diskErrors
                << "\nScenery mip chains generated/failed: " << m_scenery.generated << '/' << m_scenery.failed
                << "\nRecent Present intervals (includes loading and pauses): " << m_frameCount << '\n';
            if (m_frameCount) {
                std::vector<double> sorted;
                sorted.reserve(m_frameCount);
                for (size_t i = 0; i < m_frameCount; ++i) sorted.push_back(m_recentFrames[i].ms);
                std::sort(sorted.begin(), sorted.end());
                const double median = (sorted[(m_frameCount - 1) / 2] + sorted[m_frameCount / 2]) / 2.0;
                out << "Median/p99/max (ms): " << median << " / "
                    << sorted[m_frameCount - m_frameCount/100 - 1] << " / " << sorted.back()
                    << "\nOldest first: zone,interval_ms\n";
                const auto start = (m_frameNext + m_recentFrames.size() - m_frameCount) % m_recentFrames.size();
                for (size_t i = 0; i < m_frameCount; ++i) {
                    const auto& sample = m_recentFrames[(start + i) % m_recentFrames.size()];
                    out << sample.zone << ',' << sample.ms << '\n';
                }
            }
            out.close();
        } catch (...) {
            out.exceptions(std::ios::goodbit);
            if (out.is_open()) out.close();
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            throw;
        }
        m_nextReportTime = FrameClock::now() + std::chrono::seconds(2);
        return path;
    }

    void print_fog_status()
    {
        if (m_fogMode == FogMode::Game && !FogDistanceScale::enabled)
            chat("Game fog unavailable. Fog follows draw distance.");
        else
            chat("Fog mode: %s.", m_fogMode == FogMode::Game ? "Game" : "Distance");
    }

    void chat(const char* format, ...)
    {
        if (m_core == nullptr || m_core->GetChatManager() == nullptr)
            return;

        char message[512]{};
        va_list args;
        va_start(args, format);
        std::vsnprintf(message, sizeof(message), format, args);
        va_end(args);

        char output[640]{};
        std::snprintf(output, sizeof(output), "[VanaView] %s", message);
        m_core->GetChatManager()->Write(1, false, output);
    }

    void build_config_path()
    {
        const char* installPath = (m_core != nullptr && m_core->GetInstallPath() != nullptr) ? m_core->GetInstallPath() : ".";
        char parent[MAX_PATH]{};
        std::snprintf(parent, sizeof(parent), "%sconfig", installPath);
        ::CreateDirectoryA(parent, nullptr);
        char dir[MAX_PATH]{};
        std::snprintf(dir, sizeof(dir), "%sconfig\\plugins", installPath);
        ::CreateDirectoryA(dir, nullptr);
        std::snprintf(m_configPath, sizeof(m_configPath), "%s\\VanaView.ini", dir);
    }

    void load_config()
    {
        m_technicalDescriptions=::GetPrivateProfileIntA("interface","technical_descriptions",1,m_configPath)!=0;
        m_shadows.simple=::GetPrivateProfileIntA("shadows","simple",shadows::NormalShadows::defaultSimple,m_configPath)!=0;
        m_shadows.requested=::GetPrivateProfileIntA("shadows","full_range",1,m_configPath) ? shadows::TerrainRange::Full : shadows::TerrainRange::Native;
        const int configVersion = ::GetPrivateProfileIntA("general", "version", 0, m_configPath);
        if (configVersion < 1)
        {
            reset_config_to_defaults();
            cleanup_old_config_sections();
            save_config();
            return;
        }

        m_useZoneProfiles = ::GetPrivateProfileIntA("general", "use_zone_profiles", 0, m_configPath) != 0;
        load_global_profile(configVersion);
        m_activeZoneHasProfile = m_useZoneProfiles && m_currentZoneId != 0 && load_zone_profile(m_currentZoneId);
        save_config(false);
    }

    void load_global_profile(const int configVersion)
    {
        m_propRange = clamp_float(read_profile_float("props", "range", kDefaultPropRange), 1.0f, 4.0f);
        const int legacyMipmaps = ::GetPrivateProfileIntA("mipmaps", "enabled", 1, m_configPath);
        m_scenery.groundEnabled = ::GetPrivateProfileIntA("mipmaps", "ground", legacyMipmaps, m_configPath) != 0;
        m_scenery.foliageEnabled = ::GetPrivateProfileIntA("mipmaps", "foliage", legacyMipmaps, m_configPath) != 0;
        m_terrainGroupsEnabled = ::GetPrivateProfileIntA("terrain_groups", "enabled", 1, m_configPath) != 0;
        m_drawDistance = clamp_float(read_profile_float("draw", "distance", read_profile_float("draw", "world", default_draw_distance())), 1.0f, slider_to_draw_distance(kDistanceSliderMax));
        if (configVersion < 8)
            m_drawDistance = default_draw_distance();
        m_lodMode = static_cast<LodMode>(::GetPrivateProfileIntA("lod", "mode", static_cast<int>(LodMode::High), m_configPath));
        m_lodRangeScale = clamp_float(read_profile_float("lod", "range_scale", 1.0f), kLodRangeMin, kLodRangeMax);
        const int rawFogMode = ::GetPrivateProfileIntA("fog", "mode", static_cast<int>(FogMode::Game), m_configPath);
        m_fogMode = parse_fog_mode(rawFogMode);

        if (m_lodMode != LodMode::High)
            m_lodMode = LodMode::Off;
    }

    void save_config(const bool createZoneProfile = true)
    {
        char version[16]{};
        std::snprintf(version, sizeof(version), "%d", kConfigVersion);
        write_config_value("general", "version", version, m_configPath);
        write_config_value("general", "use_zone_profiles", m_useZoneProfiles ? "1" : "0", m_configPath);
        if (m_useZoneProfiles && m_currentZoneId != 0)
        {
            if (createZoneProfile || m_activeZoneHasProfile)
            {
                if (profile_matches_global()) {
                    char section[32]{};
                    zone_section_name(m_currentZoneId, section, sizeof(section));
                    if (write_config_value(section, nullptr, nullptr, m_configPath))
                        m_activeZoneHasProfile = false;
                    else
                        chat("Could not remove the matching zone profile.");
                    return;
                }
                save_zone_profile(m_currentZoneId);
                m_activeZoneHasProfile = true;
                return;
            }
        }
        save_global_profile();
    }

    bool profile_matches_global()
    {
        auto values = std::tie(m_drawDistance, m_propRange, m_lodMode, m_lodRangeScale,
            m_fogMode, m_terrainGroupsEnabled, m_scenery.groundEnabled, m_scenery.foliageEnabled);
        const auto saved = std::make_tuple(m_drawDistance, m_propRange, m_lodMode, m_lodRangeScale,
            m_fogMode, m_terrainGroupsEnabled, m_scenery.groundEnabled, m_scenery.foliageEnabled);
        load_global_profile(kConfigVersion);
        // INI floats are stored with three decimals. Compare at that precision.
        const bool equal = std::lround(std::get<0>(saved)*1000) == std::lround(m_drawDistance*1000) &&
            std::lround(std::get<1>(saved)*1000) == std::lround(m_propRange*1000) &&
            std::get<2>(saved) == m_lodMode &&
            std::lround(std::get<3>(saved)*1000) == std::lround(m_lodRangeScale*1000) &&
            std::get<4>(saved) == m_fogMode && std::get<5>(saved) == m_terrainGroupsEnabled &&
            std::get<6>(saved) == m_scenery.groundEnabled && std::get<7>(saved) == m_scenery.foliageEnabled;
        values = saved;
        return equal;
    }

    void save_global_profile()
    {
        write_profile_float("props", "range", m_propRange);
        write_config_value("mipmaps", "ground", m_scenery.groundEnabled ? "1" : "0", m_configPath);
        write_config_value("mipmaps", "foliage", m_scenery.foliageEnabled ? "1" : "0", m_configPath);
        write_config_value("terrain_groups", "enabled", m_terrainGroupsEnabled ? "1" : "0", m_configPath);
        write_config_value("draw", "enabled", nullptr, m_configPath);
        write_config_value("draw", "world", nullptr, m_configPath);
        write_config_value("draw", "entity", nullptr, m_configPath);
        write_profile_float("draw", "distance", m_drawDistance);
        char lodMode[16]{};
        std::snprintf(lodMode, sizeof(lodMode), "%d", static_cast<int>(m_lodMode));
        write_config_value("lod", "mode", lodMode, m_configPath);
        write_profile_float("lod", "range_scale", m_lodRangeScale);
        char mode[16]{};
        std::snprintf(mode, sizeof(mode), "%d", static_cast<int>(m_fogMode));
        write_config_value("fog", "mode", mode, m_configPath);
    }

    bool profile_section_exists(const char* section) const
    {
        char buffer[8]{};
        return ::GetPrivateProfileStringA(section, nullptr, "", buffer, sizeof(buffer), m_configPath) > 0;
    }

    void zone_section_name(const uint16_t zoneId, char* buffer, const size_t bufferSize) const
    {
        std::snprintf(buffer, bufferSize, "zone.%u", static_cast<unsigned int>(zoneId));
    }

    bool load_zone_profile(const uint16_t zoneId)
    {
        char section[32]{};
        zone_section_name(zoneId, section, sizeof(section));
        if (!profile_section_exists(section))
            return false;

        m_propRange = clamp_float(read_profile_float(section, "object_range", m_propRange), 1.0f, 4.0f);
        m_terrainGroupsEnabled = ::GetPrivateProfileIntA(section, "terrain_extension", m_terrainGroupsEnabled, m_configPath) != 0;
        m_scenery.groundEnabled = ::GetPrivateProfileIntA(section, "ground_mipmaps", m_scenery.groundEnabled, m_configPath) != 0;
        m_scenery.foliageEnabled = ::GetPrivateProfileIntA(section, "foliage_mipmaps", m_scenery.foliageEnabled, m_configPath) != 0;
        m_drawDistance = clamp_float(read_profile_float(section, "distance", m_drawDistance), 1.0f, slider_to_draw_distance(kDistanceSliderMax));
        m_lodMode = static_cast<LodMode>(::GetPrivateProfileIntA(section, "lod_mode", static_cast<int>(m_lodMode), m_configPath));
        m_lodRangeScale = clamp_float(read_profile_float(section, "lod_range", m_lodRangeScale), kLodRangeMin, kLodRangeMax);
        const int rawFogMode = ::GetPrivateProfileIntA(section, "fog_mode", static_cast<int>(m_fogMode), m_configPath);
        m_fogMode = parse_fog_mode(rawFogMode);
        if (m_lodMode != LodMode::High)
            m_lodMode = LodMode::Off;
        return true;
    }

    void save_zone_profile(const uint16_t zoneId)
    {
        char section[32]{};
        zone_section_name(zoneId, section, sizeof(section));
        write_profile_float(section, "object_range", m_propRange);
        write_config_value(section, "terrain_extension", m_terrainGroupsEnabled ? "1" : "0", m_configPath);
        write_config_value(section, "ground_mipmaps", m_scenery.groundEnabled ? "1" : "0", m_configPath);
        write_config_value(section, "foliage_mipmaps", m_scenery.foliageEnabled ? "1" : "0", m_configPath);
        write_profile_float(section, "distance", m_drawDistance);
        char lodMode[16]{};
        std::snprintf(lodMode, sizeof(lodMode), "%d", static_cast<int>(m_lodMode));
        write_config_value(section, "lod_mode", lodMode, m_configPath);
        write_profile_float(section, "lod_range", m_lodRangeScale);
        char fogMode[16]{};
        std::snprintf(fogMode, sizeof(fogMode), "%d", static_cast<int>(m_fogMode));
        write_config_value(section, "fog_mode", fogMode, m_configPath);
    }

    void reset_config_to_defaults()
    {
        m_propRange = kDefaultPropRange;
        m_terrainGroupsEnabled = true;
        m_scenery.groundEnabled = true;
        m_scenery.foliageEnabled = true;
        m_useZoneProfiles = false;
        m_activeZoneHasProfile = false;
        m_drawDistance = default_draw_distance();
        m_lodMode = LodMode::High;
        m_lodRangeScale = 1.0f;
        m_fogMode = FogMode::Game;
    }

    void cleanup_old_config_sections()
    {
        write_config_value("distance", nullptr, nullptr, m_configPath);
        write_config_value("draw", "enabled", nullptr, m_configPath);
        write_config_value("draw", "world", nullptr, m_configPath);
        write_config_value("draw", "entity", nullptr, m_configPath);
        write_config_value("fog", "scale", nullptr, m_configPath);
    }

    float read_profile_float(const char* section, const char* key, const float fallback)
    {
        char text[64]{};
        char def[64]{};
        std::snprintf(def, sizeof(def), "%.3f", static_cast<double>(fallback));
        ::GetPrivateProfileStringA(section, key, def, text, sizeof(text), m_configPath);
        return std::strtof(text, nullptr);
    }

    void write_profile_float(const char* section, const char* key, const float value)
    {
        char text[64]{};
        std::snprintf(text, sizeof(text), "%.3f", static_cast<double>(value));
        write_config_value(section, key, text, m_configPath);
    }

    void refresh_ffximain()
    {
        get_module_range("FFXiMain.dll", m_ffxiMainBase, m_ffxiMainSize);
    }

    bool refresh_drawdistance_pointers()
    {
        if (m_worldDistanceAddress != 0 && m_entityDistanceAddress != 0)
            return true;

        if (m_ffxiMainBase == 0 || m_ffxiMainSize == 0)
            refresh_ffximain();
        if (m_ffxiMainBase == 0)
            return false;

        const uint8_t signature[] = { 0x8B, 0xC1, 0x48, 0x74, 0x08, 0xD8, 0x0D };
        const uintptr_t found = find_bytes(m_ffxiMainBase, m_ffxiMainSize, signature, sizeof(signature));
        if (found == 0)
            return false;

        uint32_t worldPtr = 0;
        uint32_t entityPtr = 0;
        if (!safe_read(found + 0x07, worldPtr) || !safe_read(found + 0x0F, entityPtr))
            return false;

        m_worldDistanceAddress = worldPtr;
        m_entityDistanceAddress = entityPtr;
        return true;
    }

    bool apply_draw_distances()
    {
        if (!m_effectsEnabled) return restore_draw_distances();
        if (!capture_original_draw_distances())
            return false;
        return safe_write(m_worldDistanceAddress, m_drawDistance) &&
            safe_write(m_entityDistanceAddress, m_drawDistance);
    }

    bool capture_original_draw_distances()
    {
        if (m_originalDrawCaptured)
            return true;
        if (!refresh_drawdistance_pointers())
            return false;
        if (!safe_read(m_worldDistanceAddress, m_originalWorldDistance) ||
            !safe_read(m_entityDistanceAddress, m_originalEntityDistance))
        {
            return false;
        }
        m_originalDrawCaptured = true;
        return true;
    }

    bool restore_draw_distances()
    {
        if (!m_originalDrawCaptured)
            return true;
        if (m_worldDistanceAddress == 0 || m_entityDistanceAddress == 0)
            return true;
        return safe_write(m_worldDistanceAddress, m_originalWorldDistance) &&
            safe_write(m_entityDistanceAddress, m_originalEntityDistance);
    }

    bool install_native_fog_scale()
    {
        if (m_fogScaleWorld.patched && m_fogScaleEntity.patched) return true;
        if (!capture_original_draw_distances()) return false;
        FogDistanceScale::Sites sites{};
        const uint8_t signature[] = {0x8b,0xc1,0x48,0x74,0x08,0xd8,0x0d};
        const auto found = find_bytes(m_ffxiMainBase, m_ffxiMainSize, signature, sizeof(signature));
        if (found < m_ffxiMainBase + 0x200) return false;
        std::array<uint8_t, 0x213> code{};
        if (!safe_read(found - 0x200, code) ||
            !FogDistanceScale::locate(code.data(), code.size(), found - 0x200, sites)) return false;
        uint32_t world = 0, entity = 0;
        if (!safe_read(sites.world + 2, world) || !safe_read(sites.entity + 2, entity) ||
            world != m_worldDistanceAddress || entity != m_entityDistanceAddress) return false;
        FogDistanceScale::worldAddress = world;
        FogDistanceScale::entityAddress = entity;
        FogDistanceScale::nativeWorld = m_originalWorldDistance;
        FogDistanceScale::nativeEntity = m_originalEntityDistance;
        FogDistanceScale::worldReturn = sites.world + 6;
        FogDistanceScale::entityReturn = sites.entity + 6;
        FogDistanceScale::fogReturn0 = sites.fogReturns[0];
        FogDistanceScale::fogReturn1 = sites.fogReturns[1];
        FogDistanceScale::fogReturn2 = sites.fogReturns[2];
        if (!write_jump_patch(m_fogScaleWorld, sites.world,
                reinterpret_cast<void*>(&FogDistanceScale::world), 6) ||
            !write_jump_patch(m_fogScaleEntity, sites.entity,
                reinterpret_cast<void*>(&FogDistanceScale::entity), 6)) {
            restore_patch(m_fogScaleWorld);
            restore_patch(m_fogScaleEntity);
            return false;
        }
        return true;
    }

    void restore_default_runtime()
    {
        m_shadows.simple=shadows::NormalShadows::defaultSimple;
        m_shadows.requested=shadows::NormalShadows::defaultRange;
        m_shadowUpdate=true;
        save_shadow_settings();
        reset_config_to_defaults();
        apply_draw_distances();
        apply_lod_mode();
        apply_active_fog_mode();
        queue_mip_settings(true, true);
        set_terrain_groups_enabled(true);
        save_prop_range(kDefaultPropRange);
    }

    uint16_t get_current_zone() const
    {
        if (m_core == nullptr || m_core->GetMemoryManager() == nullptr || m_core->GetMemoryManager()->GetParty() == nullptr)
            return 0;
        return m_core->GetMemoryManager()->GetParty()->GetMemberZone(0);
    }

    void update_current_zone()
    {
        const uint16_t zoneId = get_current_zone();
        if (zoneId != 0)
            m_currentZoneId = zoneId;
    }

    void load_active_profile()
    {
        load_global_profile(kConfigVersion);
        m_activeZoneHasProfile = false;
        if (m_useZoneProfiles && m_currentZoneId != 0)
            m_activeZoneHasProfile = load_zone_profile(m_currentZoneId);
    }

    bool clear_current_zone_profile()
    {
        if (!m_useZoneProfiles || !m_currentZoneId) return false;
        char section[32]{};
        zone_section_name(m_currentZoneId, section, sizeof(section));
        if (!write_config_value(section, nullptr, nullptr, m_configPath)) {
            chat("Could not remove the zone profile.");
            return false;
        }
        load_active_profile();
        apply_loaded_profile();
        return true;
    }

    void apply_loaded_profile()
    {
        set_prop_range(m_propRange);
        apply_terrain_profile();
        apply_draw_distances();
        apply_lod_mode();
        apply_active_fog_mode();
    }

    bool local_player_ready() const
    {
        auto* memory=m_core ? m_core->GetMemoryManager() : nullptr;
        auto* party=memory ? memory->GetParty() : nullptr;
        if (!party || !party->GetMemberZone(0) || !party->GetMemberIsActive(0)) return false;
        const auto index=party->GetMemberTargetIndex(0);
        return index && memory->GetEntity() && memory->GetEntity()->GetRawEntity(index);
    }

    void schedule_zone_distance()
    {
        m_zoneDistancePending=true;
        m_zoneDistanceNextCheck=0;
        m_zoneDistanceFailures=0;
    }

    void finish_zone_distance(bool ready, uint64_t now)
    {
        if (!m_zoneDistancePending) return;
        m_zoneDistanceNextCheck=now+500;
        if (!ready) return;
        if (apply_draw_distances()) {
            m_zoneDistancePending=false;
            apply_active_fog_mode();
        } else if (++m_zoneDistanceFailures>=3) {
            m_zoneDistancePending=false;
            chat("Could not apply draw distance after entering the zone.");
        }
    }

    void update_zone_profile(uint16_t zoneId)
    {
        if (zoneId == 0) {
            if (!m_zoneDistancePending) schedule_zone_distance();
            return;
        }
        if (zoneId == m_currentZoneId) return;
        discard_ui_preview();

        if (m_useZoneProfiles && m_currentZoneId != 0 && m_activeZoneHasProfile)
            save_zone_profile(m_currentZoneId);

        m_currentZoneId = zoneId;
        m_scenery.clear();
        m_mipmaps.zone_changed();
        if (m_useZoneProfiles)
        {
            load_active_profile();
            set_prop_range(m_propRange);
            apply_lod_mode();
            apply_terrain_profile();
        }
        schedule_zone_distance();
    }

    void set_zone_profiles_enabled(const bool enabled)
    {
        if (m_useZoneProfiles == enabled)
            return;

        update_current_zone();
        if (m_useZoneProfiles && m_currentZoneId != 0 && m_activeZoneHasProfile)
            save_zone_profile(m_currentZoneId);
        if (!m_useZoneProfiles)
            save_global_profile();

        m_useZoneProfiles = enabled;
        if (m_useZoneProfiles && m_currentZoneId != 0)
        {
            load_active_profile();
            apply_loaded_profile();
        }
        else
        {
            load_global_profile(kConfigVersion);
            m_activeZoneHasProfile = false;
            apply_loaded_profile();
        }
        save_config(false);
    }

    bool install_terrain_group_hook()
    {
        if (m_terrainGroupPatch.patched)
            return true;
        if (!m_ffxiMainBase || !m_ffxiMainSize)
            refresh_ffximain();
        constexpr TerrainSignature signatures[] = {
            {TerrainGroupSelectRva, TerrainGroupSelect, sizeof(TerrainGroupSelect)},
            {TerrainGroupFallbackRva, TerrainGroupFallback, sizeof(TerrainGroupFallback)},
        };
        const auto site = terrain_site(m_ffxiMainBase, m_ffxiMainSize, TerrainGroupSelectRva, signatures);
        if (!site) {
            chat("Terrain extension unavailable for this client.");
            return false;
        }
        g_terrainGroupReturn = site + TerrainGroupHookLength;
        if (!write_jump_patch(m_terrainGroupPatch, site, reinterpret_cast<void*>(&vanaview_terrain_group_hook),
            TerrainGroupHookLength))
        {
            chat("Could not install the terrain extension.");
            return false;
        }
        return true;
    }

    void publish_terrain_group_policy()
    {
        const bool enable = m_effectsEnabled && m_terrainGroupsEnabled && !m_terrainGroupZoneExcluded;
        const LONG zone = m_terrainGroupZoneId;
        ::InterlockedExchange(&g_terrainGroupPolicy, zone && enable ? zone | 0x10000L : zone);
    }

    void update_terrain_group_zone(uint16_t zoneId)
    {
        if (m_terrainGroupZoneId == zoneId)
            return;
        m_terrainGroupZoneId = zoneId;
        char key[8]{};
        std::snprintf(key, sizeof(key), "%u", static_cast<unsigned int>(zoneId));
        m_terrainGroupZoneExcluded = zoneId &&
            ::GetPrivateProfileIntA("terrain_groups_excluded", key, 0, m_configPath) != 0;
        publish_terrain_group_policy();
    }

    void apply_terrain_profile()
    {
        if (m_terrainGroupsEnabled && !install_terrain_group_hook())
            m_terrainGroupsEnabled = false;
        update_terrain_group_zone(m_currentZoneId);
        publish_terrain_group_policy();
        m_mipStartupPending = true;
    }

    bool set_terrain_groups_enabled(bool enable)
    {
        if (enable && !install_terrain_group_hook())
            return false;
        m_terrainGroupsEnabled = enable;
        save_config();
        update_terrain_group_zone(get_current_zone());
        publish_terrain_group_policy();
        return true;
    }

    bool set_prop_range(float range)
    {
        if (!(range >= 1.0f && range <= 4.0f)) range = kDefaultPropRange;
        if (!m_effectsEnabled) { m_propRange=range; return restore_patch(m_terrainRangePatch); }
        if (range == 1.0f) {
            if (!restore_patch(m_terrainRangePatch)) return false;
        } else if (!m_terrainRangePatch.patched) {
            constexpr TerrainSignature signatures[] = {
                {0x17BF45, TerrainRangeSetup, sizeof(TerrainRangeSetup)},
                {TerrainRangeStoreRva, TerrainRangeStore, sizeof(TerrainRangeStore)},
                {0x17BF75, TerrainFarArguments, sizeof(TerrainFarArguments)},
                {0x17BFAC, TerrainFarProjectionCall, sizeof(TerrainFarProjectionCall)},
            };
            const auto site = terrain_site(m_ffxiMainBase, m_ffxiMainSize, TerrainRangeStoreRva, signatures);
            if (!site) {
                chat("Object range unavailable for this client.");
                return false;
            }
            g_terrainRangeReturn = site + sizeof(TerrainRangeStore);
            if (!write_jump_patch(m_terrainRangePatch, site, reinterpret_cast<void*>(&vanaview_terrain_range_hook),
                    sizeof(TerrainRangeStore))) return false;
        }
        g_propRangeScaleSquared = range * range;
        m_propRange = range;
        return true;
    }

    void save_prop_range(float range)
    {
        if (set_prop_range(range))
            save_config();
    }

    bool apply_lod_mode()
    {
        if (!restore_lod_patches()) return false;
        if (!m_effectsEnabled || m_lodMode == LodMode::Off)
            return true;
        if (!m_ffxiMainBase || !m_ffxiMainSize) refresh_ffximain();
        const auto shift = lod_layout_shift(m_ffxiMainBase, m_ffxiMainSize);
        if (!shift) return false;
        return m_lodMode == LodMode::High ? install_lod_high(*shift) : install_lod_range(*shift);
    }

    bool install_lod_high(uint32_t shift)
    {
        for (size_t i = 0; i < LodHighRvas.size(); ++i) {
            const uintptr_t site = m_ffxiMainBase + LodHighRvas[i] + shift;
            if (!write_bytes_patch(m_lodPatchSites[i], site, LodHighPatch[i], sizeof(LodHighPatch[i]))) {
                restore_lod_patches();
                return false;
            }
        }
        return true;
    }

    bool install_lod_range(uint32_t shift)
    {
        g_lodRangeScaleSq = m_lodRangeScale * m_lodRangeScale;
        std::array<uintptr_t, LodRangeRvas.size()> sites{};
        for (size_t i = 0; i < sites.size(); ++i) sites[i] = m_ffxiMainBase + LodRangeRvas[i] + shift;
        void* hooks[] = {
            reinterpret_cast<void*>(&vanaview_lod_range0_hook),
            reinterpret_cast<void*>(&vanaview_lod_range1_hook),
            reinterpret_cast<void*>(&vanaview_lod_range2_hook),
            reinterpret_cast<void*>(&vanaview_lod_range3_hook),
        };
        g_lodRangeReturn0 = sites[0] + sizeof(LodRangeExpected[0]);
        g_lodRangeReturn1 = sites[1] + sizeof(LodRangeExpected[1]);
        g_lodRangeReturn2 = sites[2] + sizeof(LodRangeExpected[2]);
        g_lodRangeReturn3 = sites[3] + sizeof(LodRangeExpected[3]);

        for (size_t i = 0; i < sites.size(); ++i)
        {
            if (!write_jump_patch(m_lodRangePatchSites[i], sites[i], hooks[i], sizeof(LodRangeExpected[i])))
            {
                restore_lod_patches();
                return false;
            }
        }

        return true;
    }

    bool restore_lod_patches()
    {
        const bool a = restore_patch(m_lodPatchSites[0]);
        const bool b = restore_patch(m_lodPatchSites[1]);
        bool range = true;
        for (auto& site : m_lodRangePatchSites)
            range = restore_patch(site) && range;
        return a && b && range;
    }

    TerrainMipmaps m_mipmaps;
    BytesPatchSite m_fogScaleWorld{}, m_fogScaleEntity{};
    std::mutex m_mipMutex;
    MipRequest m_mipRequest;
    bool m_mipStartupPending = false;
    IAshitaCore* m_core = nullptr;
    IDirect3DDevice8* m_device = nullptr;
    uintptr_t m_ffxiMainBase = 0;
    uint32_t m_ffxiMainSize = 0;
    uintptr_t m_worldDistanceAddress = 0;
    uintptr_t m_entityDistanceAddress = 0;
    bool m_zoneDistancePending=false;
    uint64_t m_zoneDistanceNextCheck=0;
    unsigned m_zoneDistanceFailures=0;
    bool m_originalDrawCaptured = false;
    float m_originalWorldDistance = 0.0f;
    float m_originalEntityDistance = 0.0f;
    BytesPatchSite m_lodPatchSites[2]{};
    BytesPatchSite m_lodRangePatchSites[4]{};
    BytesPatchSite m_terrainRangePatch{};
    float m_propRange = kDefaultPropRange;
    BytesPatchSite m_terrainGroupPatch{};
    bool m_terrainGroupsEnabled = false;
    uint16_t m_terrainGroupZoneId = 0;
    bool m_terrainGroupZoneExcluded = false;
    float m_drawDistance = default_draw_distance();
    LodMode m_lodMode = LodMode::High;
    float m_lodRangeScale = 1.0f;
    FogMode m_fogMode = FogMode::Game;
    bool m_activeZoneHasProfile = false;
    bool m_useZoneProfiles = false;
    uint16_t m_currentZoneId = 0;
    bool m_windowOpen = false;
    char m_configPath[MAX_PATH]{};
};

extern "C" __declspec(dllexport) double __stdcall expGetInterfaceVersion(void)
{
    return ASHITA_INTERFACE_VERSION;
}

extern "C" __declspec(dllexport) IPlugin* __stdcall expCreatePlugin(const char*)
{
    return new VanaView();
}

extern "C" __declspec(dllexport) void __stdcall expDestroyPlugin(void* instance)
{
    delete static_cast<VanaView*>(instance);
}
