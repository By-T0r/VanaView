#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../src/NormalShadows.h"
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

int wmain(int argc, wchar_t** argv)
{
    _set_error_mode(_OUT_TO_STDERR);
    assert(argc==2);
    std::ifstream file(argv[1], std::ios::binary);
    const std::vector<std::uint8_t> image(std::istreambuf_iterator<char>{file}, {});
    assert(image.size()>0x1000);
    auto header=reinterpret_cast<const IMAGE_NT_HEADERS32*>(image.data()+shadows::read<IMAGE_DOS_HEADER>(image.data()).e_lfanew);
    const auto size=header->OptionalHeader.SizeOfImage;
    assert(image.size()<=size);
    auto* mapped = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    assert(mapped);
    std::memcpy(mapped, image.data(), image.size());

    const auto dos = shadows::read<IMAGE_DOS_HEADER>(mapped);
    const auto pe = shadows::read<IMAGE_NT_HEADERS32>(mapped + dos.e_lfanew);
    const auto* sections = IMAGE_FIRST_SECTION(reinterpret_cast<const IMAGE_NT_HEADERS32*>(mapped + dos.e_lfanew));
    const auto textEnd = sections->VirtualAddress + sections->Misc.VirtualSize;
    const IMAGE_SECTION_HEADER* reloc = nullptr;
    for (WORD i = 0; i < pe.FileHeader.NumberOfSections; ++i)
        if (std::memcmp(sections[i].Name, ".reloc\0", IMAGE_SIZEOF_SHORT_NAME) == 0)
            reloc = &sections[i];
    assert(reloc);
    const auto delta = reinterpret_cast<DWORD>(mapped) - pe.OptionalHeader.ImageBase;
    // The PE directory points past the .text relocations. POL1 starts at the
    // beginning of .reloc when applying them after expanding the code.
    for (DWORD i = 0; i + sizeof(IMAGE_BASE_RELOCATION) <= reloc->Misc.VirtualSize;)
    {
        const auto* block = mapped + reloc->VirtualAddress + i;
        const auto header = shadows::read<IMAGE_BASE_RELOCATION>(block);
        if (!header.SizeOfBlock || header.VirtualAddress >= textEnd)
            break;
        assert(header.SizeOfBlock >= sizeof(header) && header.SizeOfBlock <= reloc->Misc.VirtualSize - i);
        for (DWORD j = sizeof(header); j + sizeof(WORD) <= header.SizeOfBlock; j += sizeof(WORD))
        {
            const auto entry = shadows::read<WORD>(block + j);
            // POL1 applies nonzero .text entries itself after decompression.
            const auto rva = header.VirtualAddress + (entry & 0xfff);
            if (!entry)
                continue;
            auto* address = mapped + rva;
            const auto value = shadows::read<DWORD>(address) + delta;
            std::memcpy(address, &value, sizeof(value));
        }
        i += header.SizeOfBlock;
    }

    using namespace shadows;
    assert(matches(mapped, false));
    auto* code = mapped + 0xcc000;
    const std::vector<std::uint8_t> stock(code, code + 4096);
    code[0x179] ^= 1;
    assert(set(mapped, true) == Result::Unsupported);
    code[0x179] ^= 1;
    assert(std::memcmp(code, stock.data(), stock.size()) == 0);

    DWORD protection = 0;
    assert(VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &protection));
    assert(set(mapped, true) == Result::Changed && matches(mapped, true));
    for (std::size_t i = 0; i < stock.size(); ++i)
        if (i != 0x007 && i != 0x11d && i != 0x11e)
            assert(code[i] == stock[i]);
    assert(set(mapped, true) == Result::Unsupported);
    assert(set(mapped, false) == Result::Changed && matches(mapped, false));
    assert(std::memcmp(code, stock.data(), stock.size()) == 0);
    MEMORY_BASIC_INFORMATION mem{};
    assert(VirtualQuery(code, &mem, sizeof(mem)) == sizeof(mem) && mem.Protect == PAGE_EXECUTE_READ);

    // Exercise the second patch against the same relocated client image and
    // ensure it can be toggled independently of the actor-mode patch.
    auto* renderer = mapped + 0x33000;
    const std::vector<std::uint8_t> rendererStock(renderer, renderer + 4096);
    assert(VirtualProtect(renderer, 4096, PAGE_EXECUTE_READ, &protection));
    assert(set_terrain(mapped, TerrainRange::Full, TerrainRange::Native) == Result::Unsupported);
    assert(std::memcmp(renderer, rendererStock.data(), rendererStock.size()) == 0);
    assert(set_terrain(mapped, TerrainRange::Native, TerrainRange::Full) == Result::Changed);
    assert(terrain_matches(mapped, TerrainRange::Full) && matches(mapped, false));
    assert(set(mapped, true) == Result::Changed);
    assert(terrain_matches(mapped, TerrainRange::Full) && matches(mapped, true));
    for (std::size_t i = 0; i < rendererStock.size(); ++i)
        if (i < 0xe50 || i >= 0xe54)
            assert(renderer[i] == rendererStock[i]);
    assert(set_terrain(mapped, TerrainRange::Full, TerrainRange::Native) == Result::Changed);
    assert(terrain_matches(mapped, TerrainRange::Native));
    assert(std::memcmp(renderer, rendererStock.data(), rendererStock.size()) == 0);
    assert(VirtualQuery(renderer, &mem, sizeof(mem)) == sizeof(mem) && mem.Protect == PAGE_EXECUTE_READ);
    assert(set(mapped, false) == Result::Changed);
    const auto* layout=layout_for(mapped);
    assert(layout);
    auto* setting=reinterpret_cast<DWORD*>(mapped+layout->setting);
    NormalShadows control;
    assert(!control.simple && control.requested==TerrainRange::Full);
    assert(control.sync(nullptr));
    assert(control.sync(mapped,true));
    assert(matches(mapped,false) && terrain_matches(mapped,TerrainRange::Native));
    control.simple=true;
    for (DWORD choice : {2u,0u,2u,1u,0u}) {
        *setting=choice;
        assert(control.sync(mapped));
        assert(*setting==choice && matches(mapped,choice==0));
        assert(terrain_matches(mapped,choice==0 ? TerrainRange::Full : TerrainRange::Native));
        if (choice!=0) assert(!std::memcmp(code,stock.data(),stock.size()));
    }
    assert(control.sync(mapped,true));
    assert(matches(mapped,false) && terrain_matches(mapped,TerrainRange::Native));
    assert(control.sync(mapped));
    assert(matches(mapped,true) && terrain_matches(mapped,TerrainRange::Full));
    assert(control.reset(mapped));
    assert(!std::memcmp(code,stock.data(),stock.size()) && !std::memcmp(renderer,rendererStock.data(),rendererStock.size()));
    VirtualFree(mapped, 0, MEM_RELEASE);
    std::puts("PASS: native High/Off, Normal simple shadows, terrain range, standalone conflict and restoration.");
}
