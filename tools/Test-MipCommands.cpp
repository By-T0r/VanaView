#include "../src/VanaView.cpp"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)

struct MipCommandTest {
    static void run(const char* path) {
        auto plugin=std::make_unique<VanaView>();
        // Exercise the same writer as the menu, including before the second Present.
        const std::filesystem::path folder = std::string(path) + "-reports";
        using namespace std::chrono_literals;
        const auto firstClick = VanaView::FrameClock::now();
        const auto empty = plugin->save_diagnostic_report(folder, firstClick);
        CHECK(std::filesystem::file_size(empty) > 0);
        const auto nextClick = plugin->m_nextReportTime;
        CHECK(nextClick >= firstClick + 2s && nextClick <= VanaView::FrameClock::now() + 2s);
        CHECK(plugin->save_diagnostic_report(folder, firstClick).empty());
        CHECK(plugin->save_diagnostic_report(folder, nextClick - 1ms).empty());
        CHECK(plugin->m_nextReportTime == nextClick && plugin->m_reportNumber == 1);
        auto now = VanaView::FrameClock::time_point{} + std::chrono::seconds(1);
        plugin->remember_frame(now, 1);
        plugin->m_drawDistance = 4.60f; // Between slider steps 19 and 20, closer to 20.
        // After wraparound: 450 samples at 20 ms, 449 at 40 ms and one at 90 ms.
        for (int i = 0; i < 901; ++i) {
            int interval = i <= 450 ? 20 : 40;
            if (i == 900) interval = 90;
            now += std::chrono::milliseconds(interval);
            plugin->remember_frame(now, i ? 2 : 1);
        }
        // A write failure at expiry must allow an immediate retry.
        bool failed = false;
        try { plugin->save_diagnostic_report(empty, nextClick); }
        catch (const std::filesystem::filesystem_error&) { failed = true; }
        CHECK(failed && plugin->m_nextReportTime == nextClick);
        const auto report = plugin->save_diagnostic_report(folder, nextClick);
        CHECK(report != empty);
        std::ifstream in(report);
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        CHECK(text.find("VanaView 2.10 diagnostic report") != std::string::npos);
        CHECK(text.find("pauses): 900") != std::string::npos);
        CHECK(text.find("\nDraw distance: 20  Object range: ") != std::string::npos);
        CHECK(text.find("Median/p99/max (ms): 30.00 / 40.00 / 90.00") != std::string::npos);
        CHECK(text.find("\n1,20.00") == std::string::npos && text.ends_with("2,90.00\n"));
        in.close();
        CHECK(std::filesystem::remove(report) && std::filesystem::remove(empty));
        CHECK(std::filesystem::remove(folder));
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
