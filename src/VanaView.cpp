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
#include <mutex>
#include <optional>

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
    constexpr float kDefaultPropRange = 1.0f;
    constexpr float kLodRangeMin = 0.25f;
    constexpr float kLodRangeMax = 10.0f;
    constexpr int kConfigVersion = 9;
    constexpr const char* kZoneProfilesHelp =
        "When enabled, VanaView loads a profile for the current zone if one exists. "
        "Zones without a saved profile use the global profile until you save settings for that zone.";
    constexpr const char* kDistanceHelp =
        "Raises the game's world and entity draw distance. In Distance fog mode, "
        "the game's fog range is still linked to this raised distance.";
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
    double GetVersion(void) const override { return 1.99; }

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
        m_terrainGroupsEnabled = ::GetPrivateProfileIntA("terrain_groups", "enabled", 1, m_configPath) != 0;
        if (m_terrainGroupsEnabled) {
            m_terrainGroupsEnabled = install_terrain_group_hook();
            update_terrain_group_zone(get_current_zone());
        }
        set_prop_range(read_profile_float("props", "range", kDefaultPropRange));
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
            return true;
        }

        if (iequals(sub,"grass") || iequals(sub,"flowers") || iequals(sub,"fan") || iequals(sub,"palm")) {
            const char* arg=strtok_s(nullptr," \t",&context);
            if(!arg || iequals(arg,"status")) queue_mip_settings();
            else if(iequals(arg,"on")) queue_mip_settings({}, true);
            else if(iequals(arg,"off")) queue_mip_settings({}, false);
            else chat("Use /vv %s on | off | status (shared foliage setting).", sub);
            return true;
        }
        if (iequals(sub, "help"))
        {
            print_help();
            return true;
        }

        if (iequals(sub, "status"))
        {
            print_status();
            return true;
        }

        if (iequals(sub, "proprange"))
        {
            const char* value = strtok_s(nullptr, " \t", &context);
            if (!value || iequals(value, "status"))
                chat("Object range %.2fx", double(m_propRange));
            else {
                char* end = nullptr;
                const float range = iequals(value, "off") ? 1.0f : std::strtof(value, &end);
                if ((end && (*end || end == value)) || !(range >= 1 && range <= 4))
                    chat("/vv proprange <1-4> | off | status");
                else
                    save_prop_range(range);
            }
            return true;
        }

        if (iequals(sub, "terraingroups"))
        {
            const char* mode = strtok_s(nullptr, " \t", &context);
            const char* scopeValue = strtok_s(nullptr, " \t", &context);
            if (strtok_s(nullptr, " \t", &context))
                print_terrain_group_help();
            else
                handle_terrain_groups(mode, scopeValue);
            return true;
        }

        if (iequals(sub,"coverage")) {
            const char* arg=strtok_s(nullptr," \t",&context);
            if(!arg) m_coverageRequest.store(1);
            else if(iequals(arg,"on")) m_coverageRequest.store(2);
            else if(iequals(arg,"off")) m_coverageRequest.store(3);
            else if(iequals(arg,"status")) m_coverageRequest.store(4);
            else chat("Usage: /vv coverage [on|off|status]");
            return true;
        }
        if (iequals(sub, "mip"))
        {
            const char* action = strtok_s(nullptr, " \t", &context);
            if (!action || iequals(action, "status")) {
                queue_mip_settings();
            }
            else if (iequals(action, "on")) queue_mip_settings(true, true);
            else if (iequals(action, "off")) queue_mip_settings(false, false);
            else chat("/vv mip on | off | status");
            return true;
        }

        if (iequals(sub, "reset"))
        {
            restore_default_runtime();
            save_config();
            chat("Default restored.");
            return true;
        }

        if (iequals(sub, "zone"))
        {
            const char* value = strtok_s(nullptr, " \t", &context);
            if (value == nullptr || iequals(value, "status"))
            {
                chat(
                    "Zone profiles: %s zone=%u profile=%s",
                    m_useZoneProfiles ? "on" : "off",
                    static_cast<unsigned int>(m_currentZoneId),
                    m_activeZoneHasProfile ? "zone" : "global");
                return true;
            }
            if (iequals(value, "on"))
            {
                set_zone_profiles_enabled(true);
                chat("Zone profiles enabled.");
                return true;
            }
            if (iequals(value, "off"))
            {
                set_zone_profiles_enabled(false);
                chat("Zone profiles disabled.");
                return true;
            }
            chat("Usage: /vv zone on | off | status");
            return true;
        }

        if (iequals(sub, "distance"))
        {
            const char* value = strtok_s(nullptr, " \t", &context);
            if (value == nullptr)
            {
                chat("Distance: %.0f", static_cast<double>(draw_distance_to_slider(m_drawDistance)));
                return true;
            }
            if (iequals(value, "default"))
            {
                restore_draw_distances();
                sync_default_draw_distance();
                save_config();
                chat("Draw distance restored to default.");
                return true;
            }
            char* end = nullptr;
            const float parsed = std::strtof(value, &end);
            if (end == value || *end || !std::isfinite(parsed))
            {
                chat("Usage: /vv distance <1-40> | default");
                return true;
            }
            if (parsed <= 0.0f)
            {
                restore_draw_distances();
                sync_default_draw_distance();
                save_config();
                chat("Draw distance restored to default.");
                return true;
            }
            const float distance = slider_to_draw_distance(parsed);
            m_drawDistance = distance;
            apply_draw_distances();
            save_config();
            chat("Distance set to %.0f.", static_cast<double>(draw_distance_to_slider(distance)));
            return true;
        }

        if (iequals(sub, "lod"))
        {
            const char* mode = strtok_s(nullptr, " \t", &context);
            if (mode == nullptr || iequals(mode, "status"))
            {
                print_lod_status();
                return true;
            }
            if (iequals(mode, "full") || iequals(mode, "high"))
            {
                m_lodMode = LodMode::High;
                const bool ok = apply_lod_mode();
                save_config();
                chat(ok ? "LoD mode: full." : "LoD full failed. Signature mismatch.");
                return true;
            }
            if (iequals(mode, "game") || iequals(mode, "default"))
            {
                m_lodMode = LodMode::Off;
                m_lodRangeScale = 1.0f;
                restore_lod_patches();
                save_config();
                chat("LoD restored to default.");
                return true;
            }
            chat("Usage: /vv lod full | game");
            return true;
        }

        if (iequals(sub, "fog"))
        {
            const char* mode = strtok_s(nullptr, " \t", &context);
            if (mode == nullptr || iequals(mode, "status"))
            {
                print_fog_status();
                return true;
            }
            if (iequals(mode, "distance") || iequals(mode, "linked") || iequals(mode, "default"))
            {
                set_fog_mode(FogMode::DistanceLinked);
                save_config();
                chat("Fog mode: distance-linked.");
                return true;
            }
            if (iequals(mode, "game") || iequals(mode, "natural"))
            {
                set_fog_mode(FogMode::Game);
                save_config();
                chat("Fog mode: game values.");
                return true;
            }
            chat("Usage: /vv fog game | distance | status");
            return true;
        }

        print_help();
        return true;
    }

    void Direct3DBeginScene(bool isRenderingBackBuffer) override
    {
        if (m_shadows.simple || m_shadows.requested!=shadows::TerrainRange::Native || m_shadowUpdate) {
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

        update_zone_profile(get_current_zone());
        if (m_zoneDistancePending) {
            const auto now=GetTickCount64();
            if (now>=m_zoneDistanceNextCheck)
                finish_zone_distance(local_player_ready(),now);
        }
        try {
            m_mipmaps.present(m_device);
            if(m_mipmaps.mode()==TerrainMipmaps::Mode::Auto) m_scenery.present(m_device);
        } catch (...) {
            m_mipmaps.stop();
            chat("Automatic mipmaps stopped: could not prepare the texture cache.");
        }
        process_mip_command();
        if(int request=m_coverageRequest.exchange(0)) {
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
    std::atomic<int> m_coverageRequest{0};
    SceneryMipmaps m_scenery;
    template<class Draw>
    bool draw_scenery(D3DPRIMITIVETYPE type,Draw submit) {
        if(m_mipmaps.mode()!=TerrainMipmaps::Mode::Auto || !m_currentZoneId) {
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
    void save_shadow_settings() {
        if (!::WritePrivateProfileStringA("shadows","simple",m_shadows.simple?"1":"0",m_configPath) ||
            !::WritePrivateProfileStringA("shadows","full_range",m_shadows.requested==shadows::TerrainRange::Full?"1":"0",m_configPath))
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

    void save_mip_preference(const char* key, bool enabled)
    {
        if (!::WritePrivateProfileStringA("mipmaps", key, enabled ? "1" : "0", m_configPath))
            chat("Could not save the scenery mipmap preference.");
    }

    void process_mip_command()
    {
        MipRequest request;
        {
            std::lock_guard lock(m_mipMutex);
            request=std::exchange(m_mipRequest, MipRequest{});
        }
        if (request.ground.has_value()) {
            m_scenery.groundEnabled=*request.ground;
            save_mip_preference("ground", *request.ground);
            m_mipStartupPending=true;
        }
        if (request.foliage.has_value()) {
            m_scenery.foliageEnabled=*request.foliage;
            save_mip_preference("foliage", *request.foliage);
            m_mipStartupPending=true;
        }
        if (m_device && std::exchange(m_mipStartupPending, false)) {
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
        chat("/vv proprange <1-4> | off | status");
        print_terrain_group_help();
        chat("/vv coverage [on|off|status] - highlight mipmapped draws");
        chat("/vv opens the configuration window");
        chat("/vv distance <1-40> | default");
        chat("/vv lod full | game");
        chat("/vv fog game | distance | status");
        chat("/vv zone on | off | status");
        chat("/vv reset");
        chat("/vv status");
        chat("/vv mip [on|off|status] - both ground and foliage mipmaps");
        chat("/vv grass|flowers|fan|palm on/off - shared foliage mipmap setting");
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

    void print_lod_status()
    {
        chat("LoD mode: %s range=%.2fx", lod_mode_name(), static_cast<double>(m_lodRangeScale));
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

    void set_fog_mode(const FogMode mode)
    {
        m_fogMode = mode;
        apply_active_fog_mode();
    }

    void apply_active_fog_mode()
    {
        ::InterlockedExchange(&FogDistanceScale::enabled, 0);
        if (m_fogMode == FogMode::Game) {
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

    void render_window()
    {
        if (!m_windowOpen || m_core == nullptr || m_core->GetGuiManager() == nullptr)
            return;

        auto* gui = m_core->GetGuiManager();
        const int themeCount = push_theme(gui);
        const float scale = clamp_float(gui->GetIO().DisplaySize.y / 1080.0f, 1.0f, 2.0f);
        gui->PushFont(nullptr, gui->GetStyle().FontSizeBase * scale);
        gui->SetNextWindowSize(ImVec2(450.0f * scale, 0.0f), ImGuiCond_Always);

        bool open = m_windowOpen;
        if (gui->Begin("VanaView", &open, ImGuiWindowFlags_AlwaysAutoResize))
        {
            bool changed = false;
            if (m_useZoneProfiles)
                gui->TextDisabled("Zone %u (%s)", unsigned(m_currentZoneId),
                    m_activeZoneHasProfile ? "zone profile" : "global fallback");
            gui->Text("Visibility");
            int distance = static_cast<int>(std::lround(draw_distance_to_slider(m_drawDistance)));
            gui->SetNextItemWidth(260.0f * scale);
            if (gui->SliderInt("Draw distance", &distance, 1, kDistanceSliderMax))
            {
                m_drawDistance = slider_to_draw_distance(distance);
                apply_draw_distances();
                changed = true;
            }
            show_help_marker(gui, kDistanceHelp);

            int range = static_cast<int>(std::lround(m_propRange * 4));
            char rangeLabel[24];
            std::snprintf(rangeLabel, sizeof(rangeLabel), "%.2fx", double(m_propRange));
            gui->SetNextItemWidth(260.0f * scale);
            if (gui->SliderInt("Object range", &range, 4, 16, rangeLabel, ImGuiSliderFlags_NoInput))
                save_prop_range(range * 0.25f);
            show_help_marker(gui, "Extends the distance of map objects such as trees. Higher values can reduce FPS.");
            gui->TextDisabled("Higher object range can lower FPS.");
            gui->Separator();
            gui->Text("Map LoD");
            show_help_marker(gui, kLodModeHelp);

            int lodMode = static_cast<int>(m_lodMode);
            bool lodChanged = false;
            lodChanged = gui->RadioButton("Game##lod", &lodMode, static_cast<int>(LodMode::Off));
            gui->SameLine();
            lodChanged = gui->RadioButton("Full##lod", &lodMode, static_cast<int>(LodMode::High)) || lodChanged;
            if (lodChanged)
            {
                m_lodMode = static_cast<LodMode>(lodMode);
                apply_lod_mode();
                changed = true;
            }

            gui->Separator();
            gui->Text("Fog");
            show_help_marker(gui, kFogModeHelp);

            if (gui->RadioButton("Game##fog", m_fogMode == FogMode::Game)) {
                set_fog_mode(FogMode::Game);
                changed = true;
            }
            gui->SameLine();
            if (gui->RadioButton("Distance##fog", m_fogMode == FogMode::DistanceLinked)) {
                set_fog_mode(FogMode::DistanceLinked);
                changed = true;
            }

            gui->Separator();
            gui->Text("Terrain");
            bool groups = m_terrainGroupsEnabled;
            if (gui->Checkbox("Extend distant terrain", &groups))
                set_terrain_groups_enabled(groups);
            show_help_marker(gui, "Keeps distant terrain visible when turning the camera on supported maps.");
            if (gui->Checkbox("Ground mipmaps", &m_scenery.groundEnabled)) {
                save_mip_preference("ground", m_scenery.groundEnabled);
                m_mipStartupPending=true;
            }
            show_help_marker(gui, "Ground and other scenery without an alpha cutout.");
            if (gui->Checkbox("Foliage mipmaps", &m_scenery.foliageEnabled)) {
                save_mip_preference("foliage", m_scenery.foliageEnabled);
                m_mipStartupPending=true;
            }
            show_help_marker(gui, "Grass, leaves, flowers and other scenery using the same alpha cutout.");

            gui->Separator();
            gui->Text("Shadows");
            bool shadowsChanged=gui->Checkbox("Simple actor shadows in Normal", &m_shadows.simple);
            show_help_marker(gui,"Uses simple actor shadows in the game's Normal mode. High and Off follow the game.");
            gui->Text("Terrain shadow range");
            show_help_marker(gui,"Native follows the terrain up to 20 units. Full follows it at all visible distances in Normal mode.");
            int shadowRange=static_cast<int>(m_shadows.requested);
            shadowsChanged=gui->RadioButton("Native##shadowRange",&shadowRange,static_cast<int>(shadows::TerrainRange::Native)) || shadowsChanged;
            gui->SameLine();
            shadowsChanged=gui->RadioButton("Full##shadowRange",&shadowRange,static_cast<int>(shadows::TerrainRange::Full)) || shadowsChanged;
            if (shadowsChanged) {
                m_shadows.requested=static_cast<shadows::TerrainRange>(shadowRange);
                m_shadowUpdate=true;
                save_shadow_settings();
            }
            gui->Separator();
            if (gui->CollapsingHeader("Zone settings")) {
                bool profiles = m_useZoneProfiles;
                if (gui->Checkbox("Use zone profiles", &profiles))
                    set_zone_profiles_enabled(profiles);
                show_help_marker(gui, kZoneProfilesHelp);
                const uint16_t zone = get_current_zone();
                if (zone) {
                    update_terrain_group_zone(zone);
                    bool excluded = m_terrainGroupZoneExcluded;
                    if (gui->Checkbox("Disable terrain extension here", &excluded))
                        handle_terrain_groups("zone", excluded ? "off" : "default");
                }
            }
            if (gui->Button("Restore defaults", ImVec2(0.0f, 0.0f)))
            {
                restore_default_runtime();
                changed = true;
            }

            gui->SameLine();
            if (gui->Button("Save", ImVec2(80.0f, 0.0f)))
            {
                save_config();
                chat("Settings saved.");
            }

            if (changed)
                save_config();
        }
        gui->End();
        gui->PopFont();

        m_windowOpen = open;
        if (themeCount > 0)
            gui->PopStyleColor(themeCount);
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
        m_shadows.simple=::GetPrivateProfileIntA("shadows","simple",shadows::NormalShadows::defaultSimple,m_configPath)!=0;
        m_shadows.requested=::GetPrivateProfileIntA("shadows","full_range",1,m_configPath) ? shadows::TerrainRange::Full : shadows::TerrainRange::Native;
        const int legacyMipmaps = ::GetPrivateProfileIntA("mipmaps", "enabled", 1, m_configPath);
        m_scenery.groundEnabled = ::GetPrivateProfileIntA("mipmaps", "ground", legacyMipmaps, m_configPath) != 0;
        m_scenery.foliageEnabled = ::GetPrivateProfileIntA("mipmaps", "foliage", legacyMipmaps, m_configPath) != 0;
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
        save_config(false);
    }

    void load_global_profile(const int configVersion)
    {
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
        ::WritePrivateProfileStringA("general", "version", version, m_configPath);
        ::WritePrivateProfileStringA("general", "use_zone_profiles", m_useZoneProfiles ? "1" : "0", m_configPath);
        if (m_useZoneProfiles && m_currentZoneId != 0)
        {
            if (createZoneProfile || m_activeZoneHasProfile)
            {
                save_zone_profile(m_currentZoneId);
                m_activeZoneHasProfile = true;
                return;
            }
        }
        save_global_profile();
    }

    void save_global_profile()
    {
        ::WritePrivateProfileStringA("draw", "enabled", nullptr, m_configPath);
        ::WritePrivateProfileStringA("draw", "world", nullptr, m_configPath);
        ::WritePrivateProfileStringA("draw", "entity", nullptr, m_configPath);
        write_profile_float("draw", "distance", m_drawDistance);
        char lodMode[16]{};
        std::snprintf(lodMode, sizeof(lodMode), "%d", static_cast<int>(m_lodMode));
        ::WritePrivateProfileStringA("lod", "mode", lodMode, m_configPath);
        write_profile_float("lod", "range_scale", m_lodRangeScale);
        char mode[16]{};
        std::snprintf(mode, sizeof(mode), "%d", static_cast<int>(m_fogMode));
        ::WritePrivateProfileStringA("fog", "mode", mode, m_configPath);
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
        write_profile_float(section, "distance", m_drawDistance);
        char lodMode[16]{};
        std::snprintf(lodMode, sizeof(lodMode), "%d", static_cast<int>(m_lodMode));
        ::WritePrivateProfileStringA(section, "lod_mode", lodMode, m_configPath);
        write_profile_float(section, "lod_range", m_lodRangeScale);
        char fogMode[16]{};
        std::snprintf(fogMode, sizeof(fogMode), "%d", static_cast<int>(m_fogMode));
        ::WritePrivateProfileStringA(section, "fog_mode", fogMode, m_configPath);
    }

    void reset_config_to_defaults()
    {
        m_useZoneProfiles = false;
        m_activeZoneHasProfile = false;
        m_drawDistance = default_draw_distance();
        m_lodMode = LodMode::High;
        m_lodRangeScale = 1.0f;
        m_fogMode = FogMode::Game;
    }

    void cleanup_old_config_sections()
    {
        ::WritePrivateProfileStringA("distance", nullptr, nullptr, m_configPath);
        ::WritePrivateProfileStringA("draw", "enabled", nullptr, m_configPath);
        ::WritePrivateProfileStringA("draw", "world", nullptr, m_configPath);
        ::WritePrivateProfileStringA("draw", "entity", nullptr, m_configPath);
        ::WritePrivateProfileStringA("fog", "scale", nullptr, m_configPath);
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
        ::WritePrivateProfileStringA(section, key, text, m_configPath);
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

    void sync_default_draw_distance()
    {
        if (!m_originalDrawCaptured)
            return;

        m_drawDistance = clamp_float(m_originalWorldDistance, 1.0f, slider_to_draw_distance(kDistanceSliderMax));
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

    void apply_loaded_profile()
    {
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

        if (m_useZoneProfiles && m_currentZoneId != 0 && m_activeZoneHasProfile)
            save_zone_profile(m_currentZoneId);

        m_currentZoneId = zoneId;
        m_scenery.clear();
        m_mipmaps.zone_changed();
        if (m_useZoneProfiles)
        {
            load_active_profile();
            apply_lod_mode();
        }
        schedule_zone_distance();
    }

    void set_zone_profiles_enabled(const bool enabled)
    {
        if (m_useZoneProfiles == enabled)
            return;

        update_current_zone();
        if (m_useZoneProfiles && m_currentZoneId != 0)
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

    void print_terrain_group_help()
    {
        chat("/vv terraingroups on | off | status");
        chat("/vv terraingroups zone off | default");
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
        const bool enable = m_terrainGroupsEnabled && !m_terrainGroupZoneExcluded;
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

    void handle_terrain_groups(const char* mode, const char* value)
    {
        const uint16_t zoneId = get_current_zone();
        update_terrain_group_zone(zoneId);
        if (!mode || (iequals(mode, "status") && !value))
        {
            chat("Terrain extension %s, zone %u %s.", m_terrainGroupsEnabled ? "on" : "off",
                unsigned(zoneId), m_terrainGroupZoneExcluded ? "excluded" : "follows global");
            return;
        }

        if (iequals(mode, "zone") && value && (iequals(value, "off") || iequals(value, "default")))
        {
            if (!zoneId)
            {
                chat("Enter a zone before changing its terrain groups setting.");
                return;
            }
            const bool exclude = iequals(value, "off");
            char key[8]{};
            std::snprintf(key, sizeof(key), "%u", static_cast<unsigned int>(zoneId));
            if (!::WritePrivateProfileStringA("terrain_groups_excluded", key, exclude ? "1" : nullptr, m_configPath))
            {
                chat("Could not save the terrain groups zone setting.");
                return;
            }
            m_terrainGroupZoneExcluded = exclude;
            publish_terrain_group_policy();
            chat("Terrain groups: zone %u %s.", static_cast<unsigned int>(zoneId),
                exclude ? "excluded" : "uses global setting");
            return;
        }

        if (!value && (iequals(mode, "on") || iequals(mode, "off")))
        {
            const bool enable = iequals(mode, "on");
            if (set_terrain_groups_enabled(enable))
                chat("Terrain groups: global %s%s.", enable ? "on" : "off",
                    enable && m_terrainGroupZoneExcluded ? " (current zone excluded)" : "");
            return;
        }
        print_terrain_group_help();
    }

    bool set_terrain_groups_enabled(bool enable)
    {
        if (enable && !install_terrain_group_hook())
            return false;
        if (!::WritePrivateProfileStringA("terrain_groups", "enabled", enable ? "1" : "0", m_configPath))
        {
            chat("Could not save the global terrain groups setting.");
            return false;
        }
        m_terrainGroupsEnabled = enable;
        update_terrain_group_zone(get_current_zone());
        publish_terrain_group_policy();
        return true;
    }

    bool set_prop_range(float range)
    {
        if (!(range >= 1.0f && range <= 4.0f)) range = kDefaultPropRange;
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
            write_profile_float("props", "range", m_propRange);
    }

    bool apply_lod_mode()
    {
        if (!restore_lod_patches()) return false;
        if (m_lodMode == LodMode::Off)
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
    float m_propRange = 1.0f;
    BytesPatchSite m_terrainGroupPatch{};
    bool m_terrainGroupsEnabled = false;
    uint16_t m_terrainGroupZoneId = 0;
    bool m_terrainGroupZoneExcluded = false;
    float m_drawDistance = default_draw_distance();
    enum class LodMode : int
    {
        Off = 0,
        High = 1,
        Range = 2,
    };
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
