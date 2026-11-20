#pragma once
#include <cstdint>
#include <cstring>
#include <array>

// Only the three callers that scale fog start/end use the native multipliers.
// Visibility callers and the client's weather/clamping calculations stay intact.
namespace FogDistanceScale {
inline uintptr_t worldAddress = 0, entityAddress = 0;
inline uintptr_t worldReturn = 0, entityReturn = 0;
inline uintptr_t fogReturn0 = 0, fogReturn1 = 0, fogReturn2 = 0;
inline float nativeWorld = 1, nativeEntity = 1;
inline volatile long enabled = 0;

#define FOG_SCALE_HOOK(name, address, nativeValue, continuation) \
__declspec(naked) inline void name() { \
    __asm { pushfd } \
    __asm { push eax } \
    __asm { cmp dword ptr [enabled], 0 } \
    __asm { je live_scale } \
    __asm { mov eax, dword ptr [esp + 8] } \
    __asm { cmp eax, dword ptr [fogReturn0] } \
    __asm { je native_scale } \
    __asm { cmp eax, dword ptr [fogReturn1] } \
    __asm { je native_scale } \
    __asm { cmp eax, dword ptr [fogReturn2] } \
    __asm { je native_scale } \
    __asm { live_scale: mov eax, dword ptr [address] } \
    __asm { fmul dword ptr [eax] } \
    __asm { jmp done_scale } \
    __asm { native_scale: fmul dword ptr [nativeValue] } \
    __asm { done_scale: pop eax } \
    __asm { popfd } \
    __asm { jmp dword ptr [continuation] } \
}
FOG_SCALE_HOOK(world, worldAddress, nativeWorld, worldReturn)
FOG_SCALE_HOOK(entity, entityAddress, nativeEntity, entityReturn)
#undef FOG_SCALE_HOOK

struct Sites {
    uintptr_t world = 0, entity = 0;
    std::array<uintptr_t, 3> fogReturns{};
};

inline bool locate(const uint8_t* image, size_t size, uintptr_t base, Sites& out) {
    const uint8_t prefix[] = {0x8b,0xc1,0x48,0x74,0x08,0xd8,0x0d};
    Sites result{};
    unsigned matches = 0;
    for (size_t i = 0x200; i + 19 <= size; ++i) {
        if (std::memcmp(image+i,prefix,sizeof(prefix)) || image[i+11]!=0xeb ||
            image[i+12]!=6 || image[i+13]!=0xd8 || image[i+14]!=0x0d) continue;
        // Validate the shared scaler entry as well as all three fog call tails.
        const uint8_t entry[] = {0x83,0x79,0xe4,0x01,0x74,0x09};
        const size_t function = i-0x5c;
        if (std::memcmp(image+function,entry,sizeof(entry))) continue;
        unsigned count = 0, starts = 0, ends = 0;
        for (size_t c=i-0x200; c+11<function; ++c) {
            if (image[c]!=0xe8) continue;
            int32_t relative=0; std::memcpy(&relative,image+c+1,4);
            if (int64_t(c)+5+relative != function) continue;
            const uint8_t startTail[]={0xd8,0x0f,0x8b,0xce,0xd9,0x1f};
            const uint8_t endTail[]={0xd8,0x4d,0x00,0xd9,0x5d,0x00};
            bool start=!std::memcmp(image+c+5,startTail,6);
            bool end=!std::memcmp(image+c+5,endTail,6);
            if (!start && !end) continue;
            if (count>=3) return false;
            result.fogReturns[count++]=base+c+5;
            starts+=start; ends+=end;
        }
        if (count!=3 || starts!=2 || ends!=1) continue;
        result.world=base+i+5; result.entity=base+i+13;
        ++matches;
    }
    if (matches!=1) return false;
    out=result;
    return true;
}
}
