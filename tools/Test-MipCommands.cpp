#include "../src/VanaView.cpp"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)

struct MipCommandTest {
    static void run(const char* path) {
        auto plugin=std::make_unique<VanaView>();
        // Model Ashita's documented priority ordering and stop-on-handled dispatch.
        // Both load orders must block the Lua callback, including injected commands.
        CHECK(plugin->GetPriority() < 0);
        for (bool addonLoadedFirst : {false, true}) {
            for (bool injected : {false, true}) {
                for (const char* text : {"/drawdistance setworld 20", "/drawdistance setmob 20",
                    "/drawdistance", "/drawdistance help", "  /DRAWDISTANCE\tsete 2",
                    "/addon load drawdistance", "/addon reload drawdistance",
                    "/ADDON LOAD \"DrawDistance\"", "/addon load drawdistance.lua",
                    "/addon load \"drawdistance/drawdistance.lua\""}) {
                    struct Handler { int priority; bool vanaview; };
                    std::array<Handler,2> handlers{{{plugin->GetPriority(),true},{0,false}}};
                    if (addonLoadedFirst) std::swap(handlers[0],handlers[1]);
                    std::stable_sort(handlers.begin(),handlers.end(),[](auto a,auto b){return a.priority<b.priority;});
                    bool addonCalled=false, blocked=false;
                    for (auto handler : handlers) {
                        if (handler.vanaview) blocked=plugin->HandleCommand(0,text,injected);
                        else addonCalled=true;
                        if (blocked) break;
                    }
                    CHECK(blocked && !addonCalled);
                }
            }
        }
        CHECK(plugin->HandleCommand(0,(std::string("/addon load ")+std::string(700,' ')+"drawdistance").c_str(),true));
        CHECK(!plugin->HandleCommand(0,nullptr,false));
        for (const char* text : {"", "/distance", "/drawdistance2 setworld 20",
            "/addon load distance", "/addon reload distance", "/addon unload drawdistance",
            "/addon list", "/addon load drawdistance-extra", "/echo drawdistance", "/load portfix"})
            CHECK(!plugin->HandleCommand(0,text,false));
        // Blocking never captures or writes the shared client distance values.
        CHECK(!plugin->m_originalDrawCaptured && !plugin->m_worldDistanceAddress && !plugin->m_entityDistanceAddress);
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
        CHECK(text.find("VanaView 2.12 diagnostic report") != std::string::npos);
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
        CHECK(!plugin->m_shadows.simple && plugin->m_shadows.requested==shadows::TerrainRange::Full);
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
        // Comparison bypass preserves settings and drafts; all draw entry points pass through.
        const auto beforeComparison=plugin->current_settings();
        plugin->m_terrainGroupsEnabled=false;
        plugin->m_lodMode=VanaView::LodMode::Off;
        plugin->m_propRange=1.0f;
        plugin->m_uiDraft=plugin->current_settings();
        plugin->m_uiDraft->distance=3.0f;
        const auto comparisonSettings=plugin->current_settings();
        CHECK(plugin->set_effects_enabled(false));
        CHECK(!plugin->m_effectsEnabled && plugin->current_settings()==comparisonSettings && plugin->draft_pending());
        CHECK(world==1.0f && entity==1.25f);
        CHECK(!plugin->Direct3DDrawPrimitive(D3DPT_TRIANGLELIST,0,1));
        CHECK(!plugin->Direct3DDrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,3,0,1));
        CHECK(!plugin->Direct3DDrawPrimitiveUP(D3DPT_TRIANGLELIST,1,nullptr,0));
        CHECK(!plugin->Direct3DDrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST,0,3,1,nullptr,D3DFMT_INDEX16,nullptr,0));
        plugin->m_terrainGroupZoneId=102;
        plugin->publish_terrain_group_policy();
        CHECK(!(g_terrainGroupPolicy & 0x10000));
        plugin->schedule_zone_distance();
        plugin->finish_zone_distance(true,1500);
        CHECK(world==1.0f && entity==1.25f);
        plugin->Direct3DBeginScene(true);
        CHECK(plugin->m_coverageRequest.load()==0);
        plugin->toggle_effects_from_ui(10000);
        CHECK(plugin->comparison_wait_seconds(10000)==3);
        CHECK(plugin->comparison_wait_seconds(12001)==1);
        plugin->toggle_effects_from_ui(12999);
        CHECK(plugin->m_effectsEnabled);
        CHECK(plugin->comparison_wait_seconds(13000)==0);
        CHECK(plugin->m_coverageRequest.load()==0);
        CHECK(plugin->m_effectsEnabled && plugin->current_settings()==comparisonSettings && plugin->draft_pending());
        CHECK(world==plugin->m_drawDistance && entity==plugin->m_drawDistance);
        CHECK(plugin->set_effects_enabled(false));
        plugin->update_zone_profile(101);
        CHECK(!plugin->m_effectsEnabled && world==1.0f && entity==1.25f);
        CHECK(plugin->set_effects_enabled(true));
        CHECK(plugin->m_coverageRequest.load()==0);
        plugin->m_currentZoneId=0;
        plugin->assign_settings(beforeComparison);
        plugin->m_uiDraft.reset();
        verify(true,true);
        CHECK(WritePrivateProfileStringA("mipmaps","enabled","0",path));
        CHECK(WritePrivateProfileStringA("mipmaps","ground",nullptr,path));
        CHECK(WritePrivateProfileStringA("mipmaps","foliage",nullptr,path));
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

        // Persist distinct Terrain profiles, inherit globals for old/missing zones,
        // and never leak a zone edit back into the global defaults.
        plugin->m_terrainGroupPatch.patched=true; // No client hook needed for INI/policy tests.
        plugin->m_terrainRangePatch.patched=true;
        plugin->m_propRange=1.5f;
        plugin->m_terrainGroupsEnabled=true;
        plugin->m_scenery.groundEnabled=true;
        plugin->m_scenery.foliageEnabled=false;
        plugin->save_global_profile();
        plugin->m_currentZoneId=102;
        plugin->set_zone_profiles_enabled(true);
        verify(true,false);
        CHECK(plugin->m_terrainGroupsEnabled && !plugin->m_activeZoneHasProfile);
        CHECK(plugin->set_terrain_groups_enabled(false));
        plugin->m_lodMode=VanaView::LodMode::Off;
        plugin->m_fogMode=VanaView::FogMode::Game;
        plugin->save_prop_range(2.0f);
        command("/vv mip off");
        plugin->process_mip_command();
        CHECK(GetPrivateProfileIntA("zone.102","terrain_extension",9,path)==0);
        CHECK(GetPrivateProfileIntA("zone.102","ground_mipmaps",9,path)==0);
        CHECK(GetPrivateProfileIntA("terrain_groups","enabled",9,path)==1);
        CHECK(GetPrivateProfileIntA("mipmaps","ground",9,path)==1);
        plugin->update_zone_profile(103);
        verify(true,false);
        CHECK(plugin->m_propRange==1.5f && g_propRangeScaleSquared==2.25f);
        CHECK(plugin->read_profile_float("props","range",0)==1.5f);
        CHECK(plugin->m_terrainGroupsEnabled && !plugin->m_activeZoneHasProfile);
        plugin->save_prop_range(3.0f);
        CHECK(plugin->m_mipStartupPending);
        plugin->save_mip_preference("foliage",true); // Same path as the menu.
        plugin->update_zone_profile(102);
        verify(false,false);
        CHECK(plugin->m_propRange==2.0f && g_propRangeScaleSquared==4.0f);
        CHECK(plugin->m_lodMode==VanaView::LodMode::Off && plugin->m_fogMode==VanaView::FogMode::Game);
        CHECK(!plugin->m_terrainGroupsEnabled);
        plugin->update_zone_profile(103);
        verify(true,true);
        CHECK(plugin->m_propRange==3.0f && g_propRangeScaleSquared==9.0f);
        CHECK(plugin->m_terrainGroupsEnabled);
        CHECK(WritePrivateProfileStringA("zone.104","distance","3",path));
        plugin->update_zone_profile(104);
        verify(true,false); // Old profile has no Terrain keys.
        CHECK(plugin->m_propRange==1.5f);
        CHECK(plugin->m_terrainGroupsEnabled);
        CHECK(WritePrivateProfileStringA("terrain_groups_excluded","105","1",path));
        plugin->update_zone_profile(105);
        CHECK(plugin->m_terrainGroupZoneExcluded && !(g_terrainGroupPolicy & 0x10000));
        plugin->update_zone_profile(103);
        CHECK(!plugin->m_terrainGroupZoneExcluded && (g_terrainGroupPolicy & 0x10000));
        plugin->set_zone_profiles_enabled(false);
        verify(true,false);
        CHECK(plugin->m_propRange==1.5f && g_propRangeScaleSquared==2.25f);
        CHECK(plugin->m_terrainGroupsEnabled);
        plugin->set_zone_profiles_enabled(true);
        verify(true,true);
        plugin->load_config();
        plugin->load_active_profile();
        verify(true,true); // Reload also recovers the active zone.
        CHECK(plugin->m_propRange==3.0f);
        CHECK(plugin->clear_current_zone_profile());
        CHECK(!plugin->m_activeZoneHasProfile && !plugin->profile_section_exists("zone.103"));
        verify(true,false);
        CHECK(plugin->m_propRange==1.5f && g_propRangeScaleSquared==2.25f);
        CHECK(plugin->profile_section_exists("zone.102"));
        plugin->update_zone_profile(102);
        CHECK(plugin->m_propRange==2.0f);
        plugin->update_zone_profile(103);
        CHECK(!plugin->m_activeZoneHasProfile && plugin->m_propRange==1.5f);
        plugin->save_prop_range(2.5f);
        CHECK(plugin->m_activeZoneHasProfile);
        plugin->save_prop_range(1.5f);
        CHECK(!plugin->m_activeZoneHasProfile && !plugin->profile_section_exists("zone.103"));
        plugin->set_zone_profiles_enabled(false);
        CHECK(!plugin->profile_section_exists("zone.103"));
        plugin->set_zone_profiles_enabled(true);
        plugin->save_prop_range(2.5f);
        CHECK(plugin->m_activeZoneHasProfile && plugin->profile_section_exists("zone.103"));
        plugin->save_mip_preference("foliage",true);
        plugin->update_zone_profile(0);
        CHECK(plugin->m_currentZoneId==103);
        verify(true,true);
        // UI edits are drafts: no runtime/INI change until Apply.
        plugin->m_uiDraft=plugin->current_settings();
        CHECK(!plugin->draft_pending());
        const float savedRange=plugin->m_propRange;
        plugin->m_uiDraft->range=3.25f;
        CHECK(plugin->draft_pending() && plugin->m_propRange==savedRange);
        CHECK(plugin->read_profile_float("zone.103","object_range",0)==savedRange);
        plugin->m_uiDraft->range=savedRange;
        CHECK(!plugin->draft_pending());
        plugin->m_uiDraft->range=3.25f;
        CHECK(plugin->apply_ui_draft());
        CHECK(!plugin->draft_pending() && plugin->m_propRange==3.25f);
        CHECK(plugin->read_profile_float("zone.103","object_range",0)==3.25f);
        // Interface preference survives reload without applying pending graphics.
        plugin->m_uiDraft->range=3.5f;
        CHECK(plugin->set_technical_descriptions(true));
        CHECK(plugin->read_profile_float("zone.103","object_range",0)==3.25f);
        CHECK(plugin->m_propRange==3.25f && plugin->draft_pending());
        plugin->m_technicalDescriptions=false;
        plugin->load_config();
        CHECK(plugin->m_technicalDescriptions && plugin->draft_pending());
        CHECK(plugin->set_technical_descriptions(false));
        plugin->m_technicalDescriptions=true;
        plugin->load_config();
        CHECK(!plugin->m_technicalDescriptions);
        // Immediate preview changes runtime without saving the profile.
        plugin->m_uiDraft=plugin->current_settings();
        plugin->m_uiDraft->range=3.75f;
        CHECK(plugin->preview_ui_draft());
        CHECK(plugin->m_propRange==3.75f && plugin->draft_pending());
        CHECK(plugin->read_profile_float("zone.103","object_range",0)==3.25f);
        plugin->discard_ui_preview();
        CHECK(plugin->m_propRange==3.25f && !plugin->m_uiDraft);
        plugin->m_uiDraft=plugin->current_settings();
        // Failed persistence must leave Apply enabled, even after runtime changed.
        plugin->m_uiDraft->range=3.5f;
        strcpy_s(plugin->m_configPath,"Z:\\missing-vanaview-test\\settings.ini");
        CHECK(!plugin->apply_ui_draft() && plugin->draft_pending());
        strcpy_s(plugin->m_configPath,path);
        CHECK(plugin->apply_ui_draft() && !plugin->draft_pending());
        plugin->m_uiDraft->range=4.0f;
        CHECK(plugin->preview_ui_draft());
        plugin->update_zone_profile(102);
        CHECK(!plugin->m_uiDraft && plugin->m_propRange==2.0f);
        CHECK(plugin->read_profile_float("zone.103","object_range",0)==3.5f);
        plugin->m_uiDraft=plugin->current_settings();
        plugin->m_uiDraft->range=4.0f;
        CHECK(plugin->preview_ui_draft());
        plugin->m_windowOpen=true;
        command("/vv"); // Closing restores the saved settings.
        CHECK(!plugin->m_uiDraft && plugin->m_propRange==2.0f);
        plugin->m_terrainRangePatch.patched=false;
        plugin->restore_default_runtime();
        plugin->process_mip_command();
        verify(true,true);
        CHECK(!plugin->m_useZoneProfiles && plugin->m_terrainGroupsEnabled);
        plugin->m_terrainGroupPatch.patched=false;
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
