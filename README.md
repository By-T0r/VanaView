# VanaView

VanaView is an Ashita v4 graphics plugin for Final Fantasy XI.

## What changes when you play?

- **Keep trees, rocks, and other scenery detailed as you approach.** With **Map LoD** set to **Full** (the default), the client draws visible map objects using the highest-detail model available. A tree can show its detailed branches as soon as it comes into view, avoiding the sudden change in shape or surface detail you’d normally see as you get closer.
- **See trees and bushes farther away.** **Draw distance** controls how far the client draws the world and entities. **Object range** gives map objects such as trees and bushes extra reach without changing your draw distance setting. It starts at **1×**. Raising it can reduce pop-in and make distant scenery look fuller, though it can lower your frame rate in areas with dense vegetation.
- **Reduce shimmer on ground and foliage.** Mipmaps help steady distant terrain and leaves as the camera moves. To keep the original look of the leaves, turn off **Foliage mipmaps** and leave **Ground mipmaps** on.
- **See simple character shadows follow sloping ground.** Choose **Normal** in the game settings for a simple shadow beneath each character. With **Terrain shadow range** set to **Full** (the default), those shadows follow the ground even on distant slopes. Choose **High** for detailed silhouettes or **Off** to disable character shadows.
- **Keep the game’s weather-dependent fog.** The default **Game** setting preserves the original fog distance for the current weather, even when you extend your view. You can keep draw distance at **20**, for example, and still see fog roll in as the weather changes. Choose **Distance** if you want fog to follow your draw distance setting.
- **Keep distant terrain visible as you turn the camera.** **Extend distant terrain** works across compatible maps and applies automatically as you move between zones. In **Konschtat Highlands (H-6)**, facing away from the Crag of Dem at certain angles can make the client skip a section of terrain to the northwest, leaving a sharp cutoff in the landscape even at high draw distance. In our test, the distant mountains appeared when we turned the camera to one side and disappeared when we looked straight at them. With **Extend distant terrain** enabled, the mountains stayed visible from both angles.

Open `/vv` to adjust these settings while looking at the scene. The window scales with display height, and the sliders move in fixed steps so you can easily compare settings.

**Phoenix players** should disable **Phoenix Lighting** before loading VanaView. The two plugins are incompatible.

## Install

1. Download **VanaView.dll** from the [latest release](https://github.com/By-T0r/VanaView/releases/latest).
2. Copy the DLL into the Ashita `plugins` directory. If the plugin is already loaded, run `/unload vanaview` before replacing the DLL.
3. Run `/load vanaview`, then `/vv`.

VanaView 2.0 reuses your existing ground mipmap cache. Each cache group can now keep a fifth texture, reducing the need to rebuild mipmaps when you revisit a zone. The disk limit remains 64 MiB.

## Configuration

| Control | Default | Effect |
| --- | --- | --- |
| Draw distance | 20 | Sets how far the client draws the world and entities. The slider moves in whole steps from 1 to 40. |
| Object range | 1× | Extends how far map objects such as trees and bushes are drawn. The slider moves in steps of 0.25× up to 4×. You can adjust it separately from Draw distance. Higher values can substantially reduce FPS in dense areas. |
| Map LoD | Full | Full draws the most detailed available model for visible map scenery. Game switches models at the usual distances. Older Range profiles use Game. |
| Fog | Game | Game preserves the original weather-dependent fog distance. Distance makes fog follow the draw distance setting. Existing Custom profiles use Game. |
| Extend distant terrain | On | Helps prevent distant scenery from disappearing as the camera turns. Applies on maps with compatible terrain groups and a spatial tree. |
| Ground mipmaps | On | Generates missing mipmaps for ground and other supported scenery without an alpha cutout. |
| Foliage mipmaps | On | Independently controls supported alpha-cutout scenery, including leaves, flowers, grass and objects that use the same cutout. Both settings persist. Use `/vv mip on/off` to switch both together. |
| Simple actor shadows in Normal | On | Uses simple character shadows when the game is set to Normal. High uses the game’s detailed shadows and Off disables them. |
| Terrain shadow range | Full | In Normal mode, Native uses the game’s terrain-following range. Full extends it to all visible distances. |

The default profile is global. Expand **Zone settings** to enable per-zone profiles or exclude the current zone from the terrain extension. Object range, scenery mipmaps and shadow settings apply across zones. Shadow preferences are saved when changed. Settings are stored in `config/plugins/VanaView.ini`.

**Restore defaults** asks for confirmation before resetting visibility, terrain, mipmaps and shadows and turning off zone profiles. Press **Esc** or choose **Cancel** to keep your settings. The chat command `/vv reset` applies the defaults immediately.

With dgVoodoo, set **DirectX → Mipmapping** to **App driven**. Trilinear and anisotropic 8× filtering have been tested.

Use `/vv coverage` to toggle green highlighting on scenery draws using VanaView mipmaps. Original draws retain their scene colours. Use `/vv coverage off` to end the overlay.

Use `/vv help` for chat commands. For example, `/vv proprange 1.5` sets global object range and `/vv terraingroups zone off` excludes the current zone. Use `/vv terraingroups zone default` to follow the global setting again.

## Reporting a stutter

If the game stutters, open `/vv` and click **Save diagnostic report** soon after it happens. After a successful save, the button shows **Report saved** and stays disabled for two seconds. Chat shows the filename under `logs/VanaView` in your Ashita folder. The text file includes up to the last 900 frame intervals (about 30 seconds at 30 FPS), zone IDs, graphics settings and mipmap cache counters. You can review it and attach it to your issue with a short description of what you were doing, such as entering a zone or fighting in a party. Sharing is manual.

## Scope for server administrators

**Phoenix compatibility:** VanaView and Phoenix Lighting cannot be used together.

VanaView changes rendering in the local FFXI client. Draw distance affects both scenery and entities already available to the client, so a player may see an entity at a greater distance. Object range applies to map models. The terrain extension uses the map’s existing spatial tree on compatible layouts.

The DLL modifies client memory for distance, map detail, terrain selection, fog and shadows. It uses Direct3D 8 draw callbacks to apply mipmaps to eligible scenery textures and keeps a bounded local texture cache. It reads the current zone for settings. It does not request additional entities from the server, send or modify game packets, automate player actions, or change targeting or interaction range.

The plugin entry point, commands and rendering hooks are in [VanaView.cpp](src/VanaView.cpp). The release asset can be compared against the [DLL in the tagged source tree](dist/VanaView.dll).

## Build

Set `ASHITA_SDK` to the directory containing `Ashita.h` for plugin interface 4.30. Run `./build.ps1` with Visual Studio 2022 x86 C++ build tools installed. The script builds `dist/VanaView.dll` and checks its exports and plugin interface.

Run `./build.ps1 -Test` for the command, configuration and Direct3D regressions. Optional inputs are `-D3D8` (an x86 D3D8 wrapper), `-TerrainFixtures` (the pavement DDS test set), `-FogImage` (the mapped client image for fog), and `-ShadowImages` (the mapped client images for shadows, supplied as a PowerShell array).

### Verify a release

Run `./tools/Verify-Release.ps1 -Destination ../vanaview-release-check` to rebuild and test a committed candidate in a fresh checkout. Supply the optional fixture arguments above for the extended suite. The script checks that the rebuilt DLL matches the committed DLL and prints its SHA-256.

Smoke-test the verified DLL in the client. With simple actor shadows enabled, cycle Normal → High → Off → Normal. Compare Native and Full terrain shadow range on a slope in Normal mode. Make a zone round trip with ground mipmaps enabled, save a diagnostic report and open the file named in chat. Open **Restore defaults**, press **Esc** and check that your settings are intact. When publishing, tag the verified commit and attach its `dist/VanaView.dll`.

## Credits

**rockerudon** created the original VanaView plugin. **By-T0r** develops and maintains this fork.

Released under the [MIT License](LICENSE).
