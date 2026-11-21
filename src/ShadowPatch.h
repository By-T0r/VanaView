#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace shadows
{
    static_assert(sizeof(void*) == 4, "FFXI plugins must be built for x86.");

    struct Layout {
        DWORD timestamp, imageSize, setting, distance, mode;
        DWORD nativeRange, fullRange, terrainCall, flatCall;
    };
    constexpr Layout phoenix{0x6ac37846,0xc15000,0x456acc,0x45b80c,0x45b77a,
        0x329ce0,0x34616c,0x151165,0x150df7};
    constexpr Layout retail{0x6ae818b4,0xc17000,0x457c7c,0x45c9bc,0x45c92a,
        0x32ace0,0x3472bc,0x152735,0x1523c7};
    constexpr DWORD DetailedWord = 0xcc11c;

    template<class T>
    T read(const std::uint8_t* address)
    {
        T value;
        std::memcpy(&value, address, sizeof(value));
        return value;
    }

    constexpr DWORD detailed_word(bool enabled)
    {
        // At cc11d, short jump +0x5a lands at cc179: select simple mode 0/2.
        return enabled ? 0x1d5aeb00 : 0x1d896600;
    }

    bool readable_region(const std::uint8_t* base, DWORD rva, std::size_t length)
    {
        if (!base)
            return false;
        MEMORY_BASIC_INFORMATION mem{};
        if (VirtualQuery(base + rva, &mem, sizeof(mem)) != sizeof(mem) ||
            mem.AllocationBase != base || mem.State != MEM_COMMIT ||
            (mem.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        return (base + rva - static_cast<const std::uint8_t*>(mem.BaseAddress)) + length <= mem.RegionSize;
    }

    const Layout* layout_for(const std::uint8_t* base)
    {
        if (!readable_region(base, 0, 4096)) return nullptr;
        const auto dos = read<IMAGE_DOS_HEADER>(base);
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos) ||
            dos.e_lfanew > 4096 - sizeof(IMAGE_NT_HEADERS32)) return nullptr;
        const auto pe = read<IMAGE_NT_HEADERS32>(base + dos.e_lfanew);
        if (pe.Signature != IMAGE_NT_SIGNATURE || pe.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
            pe.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return nullptr;
        for (auto* layout : {&phoenix,&retail})
            if (layout->timestamp == pe.FileHeader.TimeDateStamp && layout->imageSize == pe.OptionalHeader.SizeOfImage)
                return layout;
        return nullptr;
    }

    bool matches(const std::uint8_t* base, bool enabled)
    {
        const auto* layout=layout_for(base);
        if (!layout || !readable_region(base, 0xcc000, 0x18d) ||
            !readable_region(base, layout->setting, sizeof(DWORD)))
            return false;

        const DWORD imageBase = reinterpret_cast<DWORD>(base);
        constexpr std::uint8_t fallback[] = {0x8a, 0x8e, 0x7c, 1, 0, 0, 0xf6, 0xd9,
            0x1b, 0xc9, 0x83, 0xe1, 2, 0x66, 0x89, 0x0d};
        return read<std::uint16_t>(base + 0xcc000) == 0x3d83 &&
            read<DWORD>(base + 0xcc002) == imageBase + layout->setting &&
            base[0xcc006] == 2 && base[0xcc007] == 0x75 &&
            base[0xcc008] == 0x17 &&
            read<DWORD>(base + 0xcc00b) == imageBase + layout->distance &&
            read<DWORD>(base + 0xcc01c) == 0xfd &&
            read<DWORD>(base + 0xcc020) == 0x00bc86d9 &&
            read<DWORD>(base + DetailedWord) == detailed_word(enabled) &&
            read<DWORD>(base + 0xcc120) == imageBase + layout->mode &&
            read<std::uint16_t>(base + 0xcc124) == 0x05c7 &&
            read<DWORD>(base + 0xcc126) == imageBase + layout->distance &&
            std::memcmp(base + 0xcc179, fallback, sizeof(fallback)) == 0 &&
            read<DWORD>(base + 0xcc189) == imageBase + layout->mode;
    }

    enum class Result { Changed, Unsupported, WriteError, Conflict };

    Result set(std::uint8_t* base, bool enable)
    {
        if (!matches(base, !enable))
            return Result::Unsupported;

        auto* word = reinterpret_cast<volatile LONG*>(base + DetailedWord);
        DWORD protection=0;
        if (!VirtualProtect(base+DetailedWord,sizeof(LONG),PAGE_EXECUTE_READWRITE,&protection))
            return Result::WriteError;
        const LONG expected=static_cast<LONG>(detailed_word(!enable));
        const LONG previous=InterlockedCompareExchange(word, static_cast<LONG>(detailed_word(enable)),expected);
        FlushInstructionCache(GetCurrentProcess(),base+DetailedWord,sizeof(LONG));
        DWORD ignored=0;
        VirtualProtect(base+DetailedWord,sizeof(LONG),protection,&ignored);
        return previous==expected ? Result::Changed : Result::Conflict;
    }

    enum class TerrainRange { Native, Full };
    constexpr DWORD TerrainThresholdOperand = 0x33e50;
    DWORD range_rva(const Layout& layout,TerrainRange range) {
        if (range==TerrainRange::Full) return layout.fullRange;
        return layout.nativeRange;
    }

    bool terrain_matches(const std::uint8_t* base, TerrainRange range)
    {
        const auto* layout=layout_for(base);
        if (!layout) return false;
        const auto matches_constant = [base,layout](TerrainRange value, DWORD bits) {
            const auto rva = range_rva(*layout,value);
            return readable_region(base, rva, sizeof(DWORD)) && read<DWORD>(base + rva) == bits;
        };
        if (!readable_region(base, 0x33e48, 0x54) ||
            !matches_constant(TerrainRange::Native, 0x41a00000) ||          // 20.0f
            !matches_constant(TerrainRange::Full, 0x7f7fffff))             // FLT_MAX
            return false;

        const auto imageBase = reinterpret_cast<DWORD>(base);
        // Mode 0 loads actor distance, compares it against the threshold, then
        // selects the terrain routine below the threshold and a quad otherwise.
        constexpr std::uint8_t branch[] = {0xdf, 0xe0, 0xf6, 0xc4, 5, 0x7a, 0x1a};
        return read<WORD>(base + 0x33e48) == 0x05d9 &&
            read<DWORD>(base + 0x33e4a) == imageBase + layout->distance &&
            read<WORD>(base + 0x33e4e) == 0x1dd8 &&
            read<DWORD>(base + TerrainThresholdOperand) == imageBase + range_rva(*layout,range) &&
            std::memcmp(base + 0x33e60, branch, sizeof(branch)) == 0 &&
            base[0x33e76] == 0xe8 && read<DWORD>(base + 0x33e77) == layout->terrainCall &&
            base[0x33e94] == 0xe8 && read<DWORD>(base + 0x33e95) == layout->flatCall;
    }

    Result set_terrain(std::uint8_t* base, TerrainRange current, TerrainRange next)
    {
        if (!terrain_matches(base, current))
            return Result::Unsupported;

        // Only retarget this instruction. Other systems share the 20.0f literal.
        // All choices point inside the game image, including after plugin unload.
        const auto* layout=layout_for(base);
        auto* operand = base + TerrainThresholdOperand;
        DWORD protection = 0;
        if (!VirtualProtect(operand, sizeof(LONG), PAGE_EXECUTE_READWRITE, &protection))
            return Result::WriteError;
        const auto imageBase = reinterpret_cast<DWORD>(base);
        const auto expected = static_cast<LONG>(imageBase + range_rva(*layout,current));
        const auto replacement = static_cast<LONG>(imageBase + range_rva(*layout,next));
        const auto previous = InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(operand), replacement, expected);
        FlushInstructionCache(GetCurrentProcess(), operand, sizeof(LONG));
        DWORD ignored = 0;
        VirtualProtect(operand, sizeof(LONG), protection, &ignored);
        return previous == expected ? Result::Changed : Result::Conflict;
    }
}
