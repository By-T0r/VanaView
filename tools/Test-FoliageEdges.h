// Regression inputs are synthetic, or supplied externally; no client assets ship.
static void test_foliage_edges(IDirect3DDevice8* device)
{
    auto pixels=[](IDirect3DTexture8* t,UINT level) {
        D3DSURFACE_DESC desc{};D3DLOCKED_RECT lock{};
        CHECK(SUCCEEDED(t->GetLevelDesc(level,&desc)));
        CHECK(SUCCEEDED(t->LockRect(level,&lock,nullptr,D3DLOCK_READONLY)));
        std::vector<DWORD> result;
        for(UINT y=0;y<desc.Height;++y) {
            const auto row=reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(lock.pBits)+y*lock.Pitch);
            result.insert(result.end(),row,row+desc.Width);
        }
        CHECK(SUCCEEDED(t->UnlockRect(level)));return result;
    };
    CHECK(SUCCEEDED(device->SetPixelShader(0)));
    CHECK(SUCCEEDED(device->SetVertexShader(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX1)));
    CHECK(SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_MODULATE)));
    CHECK(SUCCEEDED(device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE)));
    CHECK(SUCCEEDED(device->SetRenderState(D3DRS_ALPHAFUNC,D3DCMP_GREATER)));
    CHECK(SUCCEEDED(device->SetRenderState(D3DRS_ALPHAREF,96)));
    auto bind=[&](SceneryMipmaps& scene,IDirect3DTexture8* source,bool cutout,Dxt3::Texture& bound) {
        CHECK(SUCCEEDED(device->SetTexture(0,source)));
        CHECK(SUCCEEDED(device->SetRenderState(D3DRS_ALPHATESTENABLE,cutout)));
        const bool used=scene.draw(device,D3DPT_TRIANGLELIST,[&] {
            Microsoft::WRL::ComPtr<IDirect3DBaseTexture8> t;
            CHECK(SUCCEEDED(device->GetTexture(0,t.GetAddressOf())));
            bound=static_cast<IDirect3DTexture8*>(t.Get());return S_OK;
        });
        Microsoft::WRL::ComPtr<IDirect3DBaseTexture8> restored;
        CHECK(SUCCEEDED(device->GetTexture(0,restored.GetAddressOf())) && restored.Get()==source);
        return used;
    };
    auto warm=[&](SceneryMipmaps& scene,IDirect3DTexture8* source,bool cutout) {
        Dxt3::Texture bound;
        for(unsigned i=0;i<1024;++i) {
            if(bind(scene,source,cutout,bound)) return bound;
            scene.present(device);
        }
        CHECK(false);return bound;
    };
    Dxt3::Texture source;
    CHECK(SUCCEEDED(device->CreateTexture(128,64,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,source.GetAddressOf())));
    D3DLOCKED_RECT lock{};CHECK(SUCCEEDED(source->LockRect(0,&lock,nullptr,0)));
    for(UINT y=0;y<64;++y) {
        auto row=reinterpret_cast<DWORD*>(static_cast<BYTE*>(lock.pBits)+y*lock.Pitch);
        for(UINT x=0;x<128;++x)
            row[x]=(y>=30 && y<34)?((x>=16 && x<20)?0xffff0000:(x>=22 && x<26)?0xff0000ff:0):0;
    }
    CHECK(SUCCEEDED(source->UnlockRect(0)));
    const auto original=pixels(source.Get(),0);
    SceneryMipmaps scene;
    auto ground=warm(scene,source.Get(),false),foliage=warm(scene,source.Get(),true);
    CHECK(pixels(foliage.Get(),0)==original && pixels(source.Get(),0)==original);
    const auto first=pixels(foliage.Get(),1),opaque=pixels(ground.Get(),1);
    CHECK(first[15*64+8]==0xffff0000 && first[16*64+11]==0xff0000ff);
    CHECK(first[15*64+10]==0x00800080); // Two red and two blue donors, alpha stays zero.
    CHECK(first[14*64+7]==0x00ff0000); // Diagonal edge.
    CHECK(first[14*64+6]==0); // No propagation through transparent space.
    for(size_t i=0;i<first.size();++i) {
        CHECK((first[i]>>24)==(opaque[i]>>24));
        if(first[i]>>24) CHECK(first[i]==opaque[i]);
    }
    CHECK(opaque[15*64+10]==0); // Ground path does not receive foliage padding.
    Dxt3::Texture bound;
    scene.clear();
    for(unsigned i=0;i<4;++i) {CHECK(!bind(scene,source.Get(),true,bound));scene.present(device);}
    scene.clear(); // Discard an incomplete chain, including padding progress.
    auto rebuilt=warm(scene,source.Get(),true);
    for(UINT l=0;l<foliage->GetLevelCount();++l) CHECK(pixels(foliage.Get(),l)==pixels(rebuilt.Get(),l));
    scene.foliageEnabled=false;CHECK(!bind(scene,source.Get(),true,bound));
    scene.foliageEnabled=true;CHECK(bind(scene,source.Get(),true,bound) && bound.Get()==rebuilt.Get());
    std::puts("PASS: foliage edge colors, alpha/top preservation, no propagation, ground isolation and interrupted rebuild.");

    // Optional independently validated expected chains, paired by source filename.
    const char* fixtures=std::getenv("VV_FOLIAGE_FIXTURES");
    const char* expected=std::getenv("VV_FOLIAGE_EXPECTED");
    if(fixtures && expected) {
        unsigned count=0;
        for(const auto& file:std::filesystem::directory_iterator(fixtures)) {
            if(!file.path().filename().string().ends_with("-source.dds")) continue;
            auto load=[&](const std::filesystem::path& path,UINT levels) {
                Dxt3::Texture texture;
                CHECK(SUCCEEDED(D3DXCreateTextureFromFileExW(device,path.c_str(),D3DX_DEFAULT,D3DX_DEFAULT,
                    levels,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,D3DX_FILTER_NONE,D3DX_FILTER_NONE,
                    0,nullptr,nullptr,texture.GetAddressOf())));
                return texture;
            };
            auto input=load(file.path(),1),golden=load(std::filesystem::path(expected)/file.path().filename(),D3DX_DEFAULT);
            SceneryMipmaps external;
            auto actual=warm(external,input.Get(),true);
            CHECK(actual->GetLevelCount()==golden->GetLevelCount());
            for(UINT l=0;l<actual->GetLevelCount();++l) CHECK(pixels(actual.Get(),l)==pixels(golden.Get(),l));
            ++count;
        }
        CHECK(count>0);std::printf("PASS: all levels match %u external expected foliage chains.\n",count);
    }
    CHECK(SUCCEEDED(device->SetTexture(0,nullptr)));
    CHECK(SUCCEEDED(device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE)));
}
