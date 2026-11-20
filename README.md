# VanaView

VanaView is an Ashita v4 graphics plugin for Final Fantasy XI.

## What changes when you play?

- **Keep trees, rocks, and other scenery detailed as you approach.** With **Map LoD** set to **Full** (the default), the client draws visible map objects using the highest-detail model available. A tree can show its detailed branches as soon as it comes into view, avoiding the sudden change in shape or surface detail you’d normally see as you get closer.
- **See trees and bushes farther away.** **Draw distance** controls how far the client draws the world and entities. **Object range** gives map objects such as trees and bushes extra reach without changing your draw distance setting. It starts at **1×**. Raising it can reduce pop-in and make distant scenery look fuller, though it can lower your frame rate in areas with dense vegetation.
- **Get a steadier view of the ground.** Terrain mipmaps reduce the shimmer you may notice on distant ground textures as you move.
- **Keep the game’s weather-dependent fog.** The default **Game** setting preserves the original fog distance for the current weather, even when you extend your view. You can keep draw distance at **20**, for example, and still see fog roll in as the weather changes. Choose **Distance** if you want fog to follow your draw distance setting.
- **Keep distant terrain visible as you turn the camera.** **Extend distant terrain** works across compatible maps and applies automatically as you move between zones. In **Konschtat Highlands (H-6)**, facing away from the Crag of Dem at certain angles can make the client skip a section of terrain to the northwest, leaving a sharp cutoff in the landscape even at high draw distance. In our test, the distant mountains appeared when we turned the camera to one side and disappeared when we looked straight at them. With **Extend distant terrain** enabled, the mountains stayed visible from both angles.

Open `/vv` to adjust these settings while looking at the scene. The window scales with display height, and the sliders move in fixed steps so you can easily compare settings.

**Phoenix players** should disable **Phoenix Lighting** before loading VanaView. The two plugins are incompatible.

## Install

1. Download **VanaView.dll** from the [latest release](https://github.com/By-T0r/VanaView/releases/latest).
2. Copy the DLL into the Ashita `plugins` directory. If updating a running client, unload VanaView first.
3. Run `/load vanaview`, then `/vv`.

## Configuration

| Control | Default | Effect |
| --- | --- | --- |
| Draw distance | 20 | Sets how far the client draws the world and entities. The slider moves in whole steps from 1 to 40. |
| Object range | 1× | Extends how far map objects such as trees and bushes are drawn. The slider moves in steps of 0.25× up to 4×. You can adjust it separately from Draw distance. Higher values can substantially reduce FPS in dense areas. |
| Map LoD | Full | Full draws the most detailed available model for visible map scenery. Game switches models at the usual distances. Older Range profiles use Game. |
| Fog | Game | Game preserves the original weather-dependent fog distance. Distance makes fog follow the draw distance setting. Existing Custom profiles use Game. |
| Extend distant terrain | On | Helps prevent distant scenery from disappearing as the camera turns. Applies on maps with compatible terrain groups and a spatial tree. |
| Terrain mipmaps | On | Reduces shimmer on supported ground textures with bump mapping enabled or disabled. |

The default profile is global. Expand **Zone settings** to enable per-zone profiles or exclude the current zone from the terrain extension. Object range and terrain mipmaps apply across zones. Settings are stored in `config/plugins/VanaView.ini`.

With dgVoodoo, set **DirectX → Mipmapping** to **App driven**. Trilinear and anisotropic 8× filtering have been tested.

Use `/vv help` for chat commands. For example, `/vv proprange 1.5` sets global object range and `/vv terraingroups zone off` excludes the current zone. Use `/vv terraingroups zone default` to follow the global setting again.

## Scope for server administrators

**Phoenix compatibility:** VanaView and Phoenix Lighting cannot be used together.

VanaView changes rendering in the local FFXI client. Draw distance affects both scenery and entities already available to the client, so a player may see an entity at a greater distance. Object range applies to map models. The terrain extension uses the map’s existing spatial tree on compatible layouts.

The DLL modifies client memory for distance, map detail, terrain selection and fog. It uses Direct3D 8 draw callbacks to apply mipmaps to eligible terrain textures and keeps a bounded local texture cache. It reads the current zone for settings. It does not request additional entities from the server, send or modify game packets, automate player actions, or change targeting or interaction range.

The plugin entry point, commands and rendering hooks are in [VanaView.cpp](src/VanaView.cpp). The release asset can be compared against the [DLL in the tagged source tree](dist/VanaView.dll).

## Build

Set `ASHITA_SDK` to the directory containing `Ashita.h` for plugin interface 4.30. Run `./build.ps1` with Visual Studio 2022 x86 C++ build tools installed. The script builds `dist/VanaView.dll` and checks its exports and plugin interface.

## Credits

**rockerudon** created the original VanaView plugin. **By-T0r** develops and maintains this fork.

Released under the [MIT License](LICENSE).
