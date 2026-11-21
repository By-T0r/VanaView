#pragma once
#include <array>
#include <bit>

// Pixel fog is applied after material shading and does not replace source alpha.
// A linear interval behind the camera gives marked fragments a uniform green.
class MipCoverage {
    struct State {D3DRENDERSTATETYPE name;DWORD value;};
    static constexpr std::array<State,5> settings={{
        {D3DRS_FOGCOLOR,0xff00ff40},
        {D3DRS_FOGSTART,std::bit_cast<DWORD>(-2.0f)},
        {D3DRS_FOGEND,std::bit_cast<DWORD>(-1.0f)},
        {D3DRS_FOGTABLEMODE,D3DFOG_LINEAR},
        {D3DRS_FOGENABLE,TRUE}
    }};
public:
    bool enabled=false;
    unsigned marked=0, errors=0;
    template<class Draw> HRESULT submit(IDirect3DDevice8* d,Draw draw) {
        if(!enabled) return draw();
        std::array<DWORD,settings.size()> saved{};
        size_t changed=0;
        for(const auto& state:settings) {
            if(FAILED(d->GetRenderState(state.name,&saved[changed])) ||
               FAILED(d->SetRenderState(state.name,state.value))) break;
            ++changed;
        }
        if(changed<settings.size()) {
            ++errors;
            while(changed) {--changed;d->SetRenderState(settings[changed].name,saved[changed]);}
            return draw();
        }
        HRESULT hr=draw();
        while(changed) {
            --changed;
            if(FAILED(d->SetRenderState(settings[changed].name,saved[changed]))) {enabled=false;++errors;}
        }
        if(SUCCEEDED(hr)) ++marked;
        return hr;
    }
};
