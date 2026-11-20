# VanaView

An Ashita v4 plugin for Final Fantasy XI. Open `/vv` to adjust visibility, terrain detail and fog. Settings apply in game.

## Install

1. Download **VanaView.dll** from the [latest release](https://github.com/By-T0r/VanaView/releases/latest).
2. Copy the DLL into the Ashita `plugins` directory. If updating a running client, unload VanaView first.
3. Run `/load vanaview`, then `/vv`.

**Phoenix Lighting is incompatible with VanaView.** Disable it before loading the plugin.

## Configuration

| Control | Default | Effect |
| --- | --- | --- |
| Draw distance | 20 | Adjusts the distance at which the client draws the world and entities. |
| Object range | 1× | Extends the range of map objects such as trees and bushes up to 4×. Higher values can substantially reduce FPS in dense areas. |
| Map LoD | Full | Keeps the highest available map detail. Game uses native detail transitions. Range lets you scale their distance. |
| Fog | Game | Game preserves the original weather-dependent fog distance. Distance makes fog follow the draw distance setting. Existing Custom profiles use Game. |
| Extend distant terrain | On | Helps prevent distant scenery from disappearing as the camera turns. Applies on maps with compatible terrain groups and a spatial tree. |
| Terrain mipmaps | On | Reduces shimmer on supported ground textures with bump mapping enabled or disabled. |

The default profile is global. Expand **Zone settings** to enable per-zone profiles or exclude the current zone from the terrain extension. Object range and terrain mipmaps apply across zones. Settings are stored in `config/plugins/VanaView.ini`.

With dgVoodoo, set **DirectX → Mipmapping** to **App driven**. Trilinear and anisotropic 8× filtering have been tested.

Use `/vv help` for chat commands. For example, `/vv proprange 1.5` sets global object range and `/vv terraingroups zone off` excludes the current zone. Use `/vv terraingroups zone default` to follow the global setting again.

## Build

Set `ASHITA_SDK` to the directory containing `Ashita.h` for plugin interface 4.30. Run `./build.ps1` with Visual Studio 2022 x86 C++ build tools installed. The script builds `dist/VanaView.dll` and checks its exports and plugin interface.

Created by **rockerudon**. This fork is maintained by **By-T0r**. Released under the [MIT License](LICENSE).
