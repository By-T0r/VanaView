#pragma once
#include "Dxt3Mipmaps.h"
#include <d3d8/includes/d3dx8.h>
#include <map>
#include <optional>
#include <tuple>
#include <filesystem>
#include <fstream>
#include <chrono>

// The client draws static MMBs with XYZ/normal/diffuse/UV and animated MMBs
// with positions and material attributes in separate streams. No asset or zone IDs.
class SceneryMipmaps {
    using Base=Microsoft::WRL::ComPtr<IDirect3DBaseTexture8>;
    using Surface=Microsoft::WRL::ComPtr<IDirect3DSurface8>;
    using Palette=std::array<PALETTEENTRY,256>;
    struct Entry {
        Base source;
        Dxt3::Texture chain;
        D3DSURFACE_DESC desc{};
        Palette palette{};
        unsigned cutoff=0, next=0;
        uint64_t seen=0, checked=0, stamp=0;
        size_t bytes=0;
        double coverage=0, scale=1;
        UINT row=0;
        bool scaling=false,padding=false;
        uint64_t passed=0;
        std::array<unsigned,256> histogram{};
        bool failed=false;
    };
    static constexpr size_t Budget=96u*1024*1024;
    using Key=std::pair<uintptr_t,unsigned>;
    std::map<Key,Entry> cache;
    size_t bytes=0;
    uint64_t frame=0;
    bool admitted=false,busy=false,restoreFailed=false;
    static bool paletted(D3DFORMAT f) { return f==D3DFMT_P8 || f==D3DFMT_A8P8; }
    static bool palette(IDirect3DDevice8* d,Palette& entries) {
        UINT n=0;
        return SUCCEEDED(d->GetCurrentTexturePalette(&n)) && SUCCEEDED(d->GetPaletteEntries(n,entries.data()));
    }
    static bool world(IDirect3DDevice8* d,DWORD& ps) {
        DWORD vs=0,z=0;
        if(FAILED(d->GetVertexShader(&vs)) || FAILED(d->GetPixelShader(&ps)) ||
           FAILED(d->GetRenderState(D3DRS_ZENABLE,&z)) || !z) return false;
        constexpr DWORD fixed=D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX1;
        if(vs==fixed || vs==(fixed-D3DFVF_TEX1+D3DFVF_TEX2)) return true;
        constexpr std::array<DWORD,7> mmb={D3DVSD_STREAM(0),D3DVSD_REG(0,D3DVSDT_FLOAT3),
            D3DVSD_STREAM(1),D3DVSD_REG(3,D3DVSDT_FLOAT3),D3DVSD_REG(5,D3DVSDT_D3DCOLOR),
            D3DVSD_REG(7,D3DVSDT_FLOAT2),D3DVSD_END()};
        std::array<DWORD,7> decl{};DWORD n=sizeof decl;
        return (vs&1) && SUCCEEDED(d->GetVertexShaderDeclaration(vs,decl.data(),&n)) && n==sizeof decl && decl==mmb;
    }
    static std::optional<unsigned> alpha_cutoff(IDirect3DDevice8* d) {
        DWORD test=0,func=0,ref=0;
        if (FAILED(d->GetRenderState(D3DRS_ALPHATESTENABLE,&test))) return std::nullopt;
        if (!test) return 0;
        if (FAILED(d->GetRenderState(D3DRS_ALPHAFUNC,&func)) ||
           FAILED(d->GetRenderState(D3DRS_ALPHAREF,&ref))) return std::nullopt;
        if(func==D3DCMP_GREATER && ref<255) return ref+1;
        if(func==D3DCMP_GREATEREQUAL && ref<=255) return ref;
        return std::nullopt;
    }
    static bool pad_lower_edges(Entry& e) {
        if(!e.next) return false;
        const UINT level=e.next;
        D3DSURFACE_DESC desc{};
        D3DLOCKED_RECT lock{};
        if(FAILED(e.chain->GetLevelDesc(e.next,&desc)) ||
           FAILED(e.chain->LockRect(e.next,&lock,nullptr,0))) return false;
        auto row=[&](UINT y){return reinterpret_cast<DWORD*>(static_cast<BYTE*>(lock.pBits)+size_t(y)*lock.Pitch);};
        const UINT end=std::min(desc.Height,e.row+std::max(1u,1024u/desc.Width));
        for(UINT y=e.row;y<end;++y) for(UINT x=0;x<desc.Width;++x) {
            auto& pixel=row(y)[x];
            if(pixel>>24) continue;
            unsigned sums[3]{},count=0;
            for(UINT yy=y?y-1:0;yy<std::min(desc.Height,y+2);++yy)
                for(UINT xx=x?x-1:0;xx<std::min(desc.Width,x+2);++xx) {
                    const DWORD neighbor=row(yy)[xx];
                    if(!(neighbor>>24)) continue;
                    ++count;
                    for(unsigned c=0;c<3;++c) sums[c]+=(neighbor>>(8*c))&255;
                }
            // Only alpha-positive donors: in-place filling cannot propagate
            // through transparent space or depend on row/chunk order.
            if(count) {
                pixel=0;
                for(unsigned c=0;c<3;++c) pixel|=((sums[c]+count/2)/count)<<(8*c);
            }
        }
        e.row=end;
        if(end==desc.Height) {++e.next;e.row=0;e.padding=false;}
        return SUCCEEDED(e.chain->UnlockRect(level));
    }
    // Downsample in premultiplied colour. For a cutout, rescale alpha toward
    // the fraction of top-level texels that pass the draw's threshold.
    static bool lower(Entry& e) {
        if(e.padding) return pad_lower_edges(e);
        UINT level=e.next;
        D3DSURFACE_DESC a{},b{};
        if(FAILED(e.chain->GetLevelDesc(level-1,&a)) || FAILED(e.chain->GetLevelDesc(level,&b))) return false;
        D3DLOCKED_RECT src{},dst{};
        if(FAILED(e.chain->LockRect(level-1,&src,nullptr,D3DLOCK_READONLY))) return false;
        if(FAILED(e.chain->LockRect(level,&dst,nullptr,0))) { e.chain->UnlockRect(level-1);return false; }
        auto row=[](const D3DLOCKED_RECT& r,UINT y){return reinterpret_cast<DWORD*>(static_cast<BYTE*>(r.pBits)+size_t(y)*r.Pitch);};
        // Bound each downsampling or alpha-adjustment step by output texels.
        UINT end=std::min(b.Height,e.row+std::max(1u,8192u/b.Width));
        if(!e.scaling) for(UINT y=e.row;y<end;++y) for(UINT x=0;x<b.Width;++x) {
            unsigned channels[3]{},weights=0,alphaSum=0,samples=0;
            for(UINT sy=y*a.Height/b.Height;sy<(y+1)*a.Height/b.Height;++sy)
                for(UINT sx=x*a.Width/b.Width;sx<(x+1)*a.Width/b.Width;++sx) {
                    DWORD pixel=row(src,sy)[sx];unsigned alpha=pixel>>24;
                    if(level==1 && e.cutoff) e.passed+=alpha>=e.cutoff;
                    unsigned weight=e.cutoff?alpha:1;
                    weights+=weight;
                    alphaSum+=alpha;
                    ++samples;
                    for(unsigned c=0;c<3;++c) channels[c]+=((pixel>>(8*c))&255)*weight;
                }
            unsigned alpha=(alphaSum+samples/2)/samples; ++e.histogram[alpha];
            DWORD pixel=alpha<<24;
            if(weights) for(unsigned c=0;c<3;++c) pixel|=((channels[c]+weights/2)/weights)<<(8*c);
            row(dst,y)[x]=pixel;
        }
        if(e.cutoff && !e.scaling && end==b.Height) {
            if(level==1) e.coverage=double(e.passed)/(a.Width*a.Height);
            const double wanted=e.coverage*b.Width*b.Height;
            unsigned n=0;
            for(unsigned k=e.cutoff;k<256;++k)n+=e.histogram[k];
            double best=std::abs(double(n)-wanted),scale=1; n=0;
            for(unsigned k=255;k>0;--k) {
                n+=e.histogram[k];double err=std::abs(double(n)-wanted);
                if(e.histogram[k] && err<best){best=err;scale=(e.cutoff-0.49)/k;}
            }
            e.scale=scale;
            if(scale!=1) {e.scaling=true;e.row=0;end=0;}
        } else if(e.scaling) {
            for(UINT y=e.row;y<end;++y) for(UINT x=0;x<b.Width;++x) {
                auto& px=row(dst,y)[x];
                px=(px&0xffffff)|(DWORD(std::min(255.,(px>>24)*e.scale+0.5))<<24);
            }
        }
        e.row=end;
        if(end==b.Height) {
            if(e.cutoff) e.padding=true;
            else ++e.next;
            e.row=0;e.scaling=false;e.histogram={};
        }
        HRESULT h=e.chain->UnlockRect(level);
        return SUCCEEDED(e.chain->UnlockRect(level-1)) && SUCCEEDED(h);
    }
    static bool fingerprint(const Entry& e,uint64_t& hash) {
        UINT rows=e.desc.Height;
        UINT pitch=0;
        switch(e.desc.Format) {
        case D3DFMT_DXT1: case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
            pitch=((e.desc.Width+3)/4)*(e.desc.Format==D3DFMT_DXT1?8:16);rows=(rows+3)/4;break;
        case D3DFMT_A8: case D3DFMT_P8: case D3DFMT_L8: case D3DFMT_A4L4: case D3DFMT_R3G3B2:
            pitch=e.desc.Width;break;
        case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
        case D3DFMT_X4R4G4B4: case D3DFMT_A8P8: case D3DFMT_A8L8: case D3DFMT_A8R3G3B2:
            pitch=e.desc.Width*2;break;
        case D3DFMT_R8G8B8: pitch=e.desc.Width*3;break;
        case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A2B10G10R10: case D3DFMT_G16R16:
            pitch=e.desc.Width*4;break;
        default: return false;
        }
        auto t=static_cast<IDirect3DTexture8*>(e.source.Get());
        D3DLOCKED_RECT lock{};
        if(FAILED(t->LockRect(0,&lock,nullptr,D3DLOCK_READONLY))) return false;
        hash=14695981039346656037ULL;
        bool valid=lock.pBits && lock.Pitch>=static_cast<INT>(pitch);
        if(valid) for(UINT y=0;y<rows;++y) {
            const BYTE* row=static_cast<const BYTE*>(lock.pBits)+size_t(y)*lock.Pitch;
            // Hash whole words where possible, including the tail on narrow maps.
            UINT x=0;
            for(;x+4<=pitch;x+=4){DWORD word;std::memcpy(&word,row+x,4);hash=(hash^word)*1099511628211ULL;}
            for(;x<pitch;++x) hash=(hash^row[x])*1099511628211ULL;
        }
        return SUCCEEDED(t->UnlockRect(0)) && valid;
    }
    static void restart(Entry& e) {
        e.chain.Reset();e.next=0;e.row=0;e.histogram={};e.scaling=false;e.padding=false;e.passed=0;
    }
    static bool top(IDirect3DDevice8* d,Entry& e) {
        Surface from,to;
        return fingerprint(e,e.stamp) && SUCCEEDED(d->CreateTexture(e.desc.Width,e.desc.Height,0,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,e.chain.GetAddressOf())) &&
            e.chain->GetLevelCount()>1 &&
            SUCCEEDED(static_cast<IDirect3DTexture8*>(e.source.Get())->GetSurfaceLevel(0,from.GetAddressOf())) &&
            SUCCEEDED(e.chain->GetSurfaceLevel(0,to.GetAddressOf())) &&
            SUCCEEDED(D3DXLoadSurfaceFromSurface(to.Get(),nullptr,nullptr,from.Get(),
                paletted(e.desc.Format)?e.palette.data():nullptr,nullptr,D3DX_FILTER_NONE,0));
    }
    Entry* select(IDirect3DDevice8* d,IDirect3DBaseTexture8* key,unsigned cutoff) {
        Key id{reinterpret_cast<uintptr_t>(key),cutoff};
        auto it=cache.find(id);
        if(it!=cache.end()) {
            Entry& e=it->second; e.seen=frame;
            if(e.failed || !e.chain || e.next<e.chain->GetLevelCount() || frame-e.checked>480 || key->GetLOD()!=0) return nullptr;
            if(paletted(e.desc.Format)) {
                Palette current;
                if(!palette(d,current)) return nullptr;
                if(std::memcmp(current.data(),e.palette.data(),sizeof current)) {
                    e.palette=current;restart(e);return nullptr;
                }
            }
            return &e;
        }
        if(admitted || key->GetType()!=D3DRTYPE_TEXTURE || key->GetLevelCount()!=1 || key->GetLOD()!=0) return nullptr;
        D3DSURFACE_DESC desc{};
        if(FAILED(static_cast<IDirect3DTexture8*>(key)->GetLevelDesc(0,&desc)) || desc.Usage ||
           desc.Pool!=D3DPOOL_MANAGED || (!desc.Width || !desc.Height) ||
           (desc.Width==1 && desc.Height==1)) return nullptr;
        size_t cost=0;
        for(UINT w=desc.Width,h=desc.Height;;w=std::max(1u,w/2),h=std::max(1u,h/2)){
            if(uint64_t(w)*h*4>Budget-cost) return nullptr;
            cost+=size_t(w)*h*4;if(w==1 && h==1) break;
        }
        while(!cache.empty() && (cache.size()>=256 || cost>Budget-bytes)) {
            auto oldest=std::min_element(cache.begin(),cache.end(),[](auto& a,auto& b){return a.second.seen<b.second.seen;});
            if(frame-oldest->second.seen<60) return nullptr;
            bytes-=oldest->second.bytes;cache.erase(oldest);
        }
        Entry e;e.source=key;e.desc=desc;e.bytes=cost;e.seen=frame;e.cutoff=cutoff;
        if(paletted(desc.Format) && !palette(d,e.palette)) return nullptr;
        cache.emplace(id,std::move(e));bytes+=cost;admitted=true;
        return nullptr;
    }
public:
    bool groundEnabled=true, foliageEnabled=true;
    uint64_t generated=0,draws=0,cutouts=0,failed=0;
    bool pending() const {
        for (const auto& [key,e] : cache) {
            if (e.failed || (e.cutoff && !foliageEnabled)) continue;
            if (!e.chain || e.next<e.chain->GetLevelCount()) return true;
        }
        return false;
    }
    void clear(){
        cache.clear();bytes=0;frame=generated=draws=cutouts=failed=0;admitted=false;restoreFailed=false;}
    void present(IDirect3DDevice8* d) {
        ++frame;admitted=false;
        // Do at most one level per present, prioritising visible materials.
        const auto priority = [drawFrame = frame - 1](const Entry& entry) {
            return std::make_tuple(entry.seen == drawFrame, entry.next, entry.seen);
        };
        Entry* next=nullptr;
        Entry* check=nullptr;
        for(auto& item:cache) {
            Entry& e=item.second;
            // Cutoff zero also serves secondary texture stages on foliage draws.
            if (e.failed || (e.cutoff && !foliageEnabled)) continue;
            if(e.chain && e.next>=e.chain->GetLevelCount()) {
                if(frame-e.seen<60 && frame-e.checked>240 && (!check || e.checked<check->checked)) check=&e;
                continue;
            }
            if (!next || priority(e) > priority(*next)) next = &e;
        }
        if(check) {
            uint64_t stamp=0;
            if(!fingerprint(*check,stamp) || stamp!=check->stamp) restart(*check);
            check->checked=frame;
        }
        if(!next) return;
        bool ok=next->next?lower(*next):top(d,*next);
        if(!ok){
            next->chain.Reset();next->failed=true;++failed;return;}
        if(!next->next) next->next=1;
        if(next->next==next->chain->GetLevelCount()) {
            uint64_t stamp=0;
            if(!fingerprint(*next,stamp) || stamp!=next->stamp) restart(*next);
            else {next->checked=frame;++generated;
            }
        }
    }
    template<class Draw> bool draw(IDirect3DDevice8* d,D3DPRIMITIVETYPE type,Draw submit,unsigned* levels=nullptr) {
        DWORD ps=0;
        if(busy || restoreFailed || !d || (type!=D3DPT_TRIANGLELIST && type!=D3DPT_TRIANGLESTRIP && type!=D3DPT_TRIANGLEFAN) || !world(d,ps)) return false;
        struct Binding {Base original; Dxt3::Texture replacement; DWORD stage,filter;bool filterChanged=false;};
        std::array<Binding,8> saved{};
        const auto threshold=alpha_cutoff(d);
        if (!threshold) return false;
        const unsigned cutoff=*threshold;
        unsigned used=0;
        if (!(cutoff ? foliageEnabled : groundEnabled)) return false;
        for(DWORD stage=0;stage<saved.size();++stage) {
            DWORD op=0,filter=0;
            if(!ps && (FAILED(d->GetTextureStageState(stage,D3DTSS_COLOROP,&op)) || op==D3DTOP_DISABLE)) break;
            Base t;
            if(FAILED(d->GetTexture(stage,t.GetAddressOf())) || !t ||
               FAILED(d->GetTextureStageState(stage,D3DTSS_MIPFILTER,&filter))) continue;
            Entry* e=select(d,t.Get(),stage==0?cutoff:0);
            if(!e) continue;
            // Finish all selecting (which can allocate) before changing device state.
            saved[used++]={t,e->chain,stage,filter,false};
        }
        if(!used) return false;
        unsigned bound=0;
        for(;bound<used;++bound) {
            auto& b=saved[bound];
            if(FAILED(d->SetTexture(b.stage,b.replacement.Get()))) break;
            if(b.filter==D3DTEXF_NONE) {
                if(FAILED(d->SetTextureStageState(b.stage,D3DTSS_MIPFILTER,D3DTEXF_LINEAR))) {
                    if(FAILED(d->SetTexture(b.stage,b.original.Get()))) {++failed;restoreFailed=true;}
                    break;
                }
                b.filterChanged=true;
            }
        }
        bool drawn=false;
        if(bound==used){busy=true;drawn=SUCCEEDED(submit());busy=false;}
        while(bound){
            auto& b=saved[--bound];
            if(b.filterChanged && FAILED(d->SetTextureStageState(b.stage,D3DTSS_MIPFILTER,b.filter))) {
                restoreFailed=true;++failed;
            }
            if(FAILED(d->SetTexture(b.stage,b.original.Get()))) {restoreFailed=true;++failed;}
        }
        if(drawn){++draws;if(cutoff)++cutouts;
            if(levels && saved[0].stage==0)*levels=saved[0].replacement->GetLevelCount();}
        return drawn;
    }
};
