#include "../src/VanaView.cpp"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)

struct MipCommandTest {
    static void run(const char* path) {
        auto plugin=std::make_unique<VanaView>();
        strcpy_s(plugin->m_configPath,path);
        auto verify=[&](bool ground, bool foliage) {
            CHECK(plugin->m_scenery.groundEnabled==ground);
            CHECK(plugin->m_scenery.foliageEnabled==foliage);
        };
        auto command=[&](const std::string& text) {
            CHECK(plugin->HandleCommand(0,text.c_str(),false));
        };
        plugin->load_config();
        CHECK(plugin->m_shadows.simple && plugin->m_shadows.requested==shadows::TerrainRange::Full);
        plugin->m_shadows.simple=false;
        plugin->m_shadows.requested=shadows::TerrainRange::Native;
        plugin->save_shadow_settings();
        plugin->m_shadows.simple=true;
        plugin->m_shadows.requested=shadows::TerrainRange::Full;
        plugin->load_config();
        CHECK(!plugin->m_shadows.simple && plugin->m_shadows.requested==shadows::TerrainRange::Native);
        float world=1.0f, entity=1.25f;
        plugin->m_worldDistanceAddress=reinterpret_cast<uintptr_t>(&world);
        plugin->m_entityDistanceAddress=reinterpret_cast<uintptr_t>(&entity);
        plugin->m_drawDistance=4.646f;
        plugin->m_fogMode=VanaView::FogMode::DistanceLinked;
        plugin->schedule_zone_distance();
        plugin->finish_zone_distance(false,0);
        CHECK(world==1.0f && entity==1.25f && !plugin->m_originalDrawCaptured);
        plugin->finish_zone_distance(true,500);
        CHECK(!plugin->m_zoneDistancePending && world==4.646f && entity==4.646f);
        CHECK(plugin->restore_draw_distances() && world==1.0f && entity==1.25f);
        plugin->finish_zone_distance(true,1000);
        CHECK(world==1.0f && entity==1.25f);
        verify(true,true);
        CHECK(WritePrivateProfileStringA("mipmaps","enabled","0",path));
        plugin->load_config();
        verify(false,false);
        for (const char* alias : {"grass","flowers","fan","palm"}) {
            const std::string prefix=std::string("/vv ")+alias;
            command("/vv mip on");
            command(prefix+" off");
            command(prefix+" status");
            plugin->process_mip_command();
            verify(true,false);
            plugin->load_config();
            verify(true,false);

            command("/vv mip off");
            command(prefix+" on");
            command("/vv mip status");
            plugin->process_mip_command();
            verify(false,true);
            plugin->load_config();
            verify(false,true);
        }
        command("/vv palm off");
        command("/vv mip on");
        plugin->process_mip_command();
        verify(true,true);
        command("/vv mip off");
        plugin->process_mip_command();
        verify(false,false);
        const auto packetFlag=static_cast<uint32_t>(Ashita::PluginFlags::UsePackets);
        CHECK(!(plugin->GetFlags() & packetFlag));
        command("/vv coverage trace");
        CHECK(plugin->m_coverageRequest.load()==0);
        command("/vv coverage on");
        CHECK(plugin->m_coverageRequest.load()==2);
    }
};

int main() {
    char dir[MAX_PATH]{}, file[MAX_PATH]{};
    CHECK(GetTempPathA(MAX_PATH,dir) && GetTempFileNameA(dir,"vvm",0,file));
    CHECK(WritePrivateProfileStringA("general","version","3",file));
    MipCommandTest::run(file);
    CHECK(DeleteFileA(file));
    std::puts("PASS: four foliage aliases, global commands, status, profile capabilities, defaults and INI reload.");
}
