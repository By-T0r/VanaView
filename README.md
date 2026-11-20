# VanaView

VanaView is an Ashita v4 graphics plugin for Final Fantasy XI.

## What changes when you play?

- **Look across a zone.** More of the landscape can stay in view as you travel. On supported maps, turning the camera can keep a distant ridge in sight that would otherwise disappear at certain angles.
- **See trees and bushes farther away.** Draw distance controls the overall view. Object range gives scenery such as trees an extra reach of its own. It starts at 1×. Raising it can lower your frame rate in areas with dense vegetation.
- **Get a steadier view of the ground.** Terrain mipmaps reduce the shimmer you can see on distant ground textures while moving.
- **Keep the weather's fog.** The default Game setting preserves the fog distance for the current weather, even when you extend the view. Choose Distance if you want fog to follow your draw distance.

Open `/vv` to adjust these settings while looking at the scene. The window scales with display height, and the sliders move in fixed steps for easy comparisons.

**Phoenix players:** VanaView is incompatible with **Phoenix Lighting**. Disable Phoenix Lighting before loading VanaView.

## Install

1. Download **VanaView.dll** from the [latest release](https://github.com/By-T0r/VanaView/releases/latest).
2. Copy the DLL into the Ashita `plugins` directory. If updating a running client, unload VanaView first.
3. Run `/load vanaview`, then `/vv`.

## Configuration

| Control | Default | Effect |
| --- | --- | --- |
| Draw distance | 20 | Uses whole steps from 1 to 40. Adjusts the distance at which the client draws the world and entities. |
| Object range | 1× | Extends the range of map objects such as trees and bushes up to 4× in steps of 0.25× in the window. Higher values can substantially reduce FPS in dense areas. |
| Map LoD | Full | Keeps the highest available map detail. Game uses native detail transitions. Older Range profiles use Game. |
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

Created by **rockerudon**. This fork is maintained by **By-T0r**. Released under the [MIT License](LICENSE).
