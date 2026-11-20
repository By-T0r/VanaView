#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
#include "../src/FogDistanceScale.h"
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
__declspec(naked) void continuation() { __asm { ret } }
static void check_scale(bool entity, bool fog, unsigned slot, bool enabled) {
    float liveWorld=4, liveEntity=6;
    FogDistanceScale::worldAddress=reinterpret_cast<uintptr_t>(&liveWorld);
    FogDistanceScale::entityAddress=reinterpret_cast<uintptr_t>(&liveEntity);
    FogDistanceScale::nativeWorld=1; FogDistanceScale::nativeEntity=2;
    FogDistanceScale::worldReturn=FogDistanceScale::entityReturn=reinterpret_cast<uintptr_t>(&continuation);
    FogDistanceScale::fogReturn0=FogDistanceScale::fogReturn1=FogDistanceScale::fogReturn2=0;
    uintptr_t* slots[]={&FogDistanceScale::fogReturn0,&FogDistanceScale::fogReturn1,&FogDistanceScale::fogReturn2};
    uintptr_t* target=slots[slot];
    auto hook=entity ? &FogDistanceScale::entity : &FogDistanceScale::world;
    FogDistanceScale::enabled=enabled;
    float input=7, result=0;
    DWORD actualEax=0, flags=0;
    __asm {
        cmp fog, 0
        je skip_address
        lea eax, resumed
        mov edx, target
        mov [edx], eax
    skip_address:
        fld input
        mov eax, 12345678h
        cmp eax, eax
        call hook
    resumed:
        pushfd
        pop edx
        mov flags, edx
        mov actualEax, eax
        fstp result
    }
    CHECK(actualEax==0x12345678 && (flags & 0x40));
    CHECK(result==7*((fog && enabled) ? (entity?2:1) : (entity?6:4)));
    CHECK(liveWorld==4 && liveEntity==6);
}
int main(int argc,char**argv) {
    CHECK(argc==2);
    std::ifstream f(argv[1],std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)),{});
    FogDistanceScale::Sites sites{};
    CHECK(FogDistanceScale::locate(b.data(),b.size(),0x10000000,sites));
    CHECK(sites.world==0x10189311 && sites.entity==0x10189319);
    CHECK(sites.fogReturns[0]==0x101891f0 && sites.fogReturns[1]==0x10189268 && sites.fogReturns[2]==0x10189281);
    CHECK(!FogDistanceScale::locate(b.data(),128,0x10000000,sites));
    b[0x189281]^=1;
    CHECK(!FogDistanceScale::locate(b.data(),b.size(),0x10000000,sites));
    for(unsigned slot=0;slot<3;++slot)
        for(bool entity:{false,true}) for(bool fog:{false,true}) for(bool enabled:{false,true})
            check_scale(entity,fog,slot,enabled);
    std::puts("PASS: validated client callsites, mismatch rejection, all fog callers, visibility passthrough, mode toggle, x87 result and EAX/flags preservation.");
}
