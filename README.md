# VanaView

VanaView is a graphics plugin for Final Fantasy XI on Ashita v4. It lets you see scenery farther away, keeps visible objects detailed as you approach, and reduces texture shimmer on ground and foliage. You can also adjust fog and simple character shadows in game.

> **Phoenix players.** VanaView is approved for use on the Phoenix server. Disable **Phoenix Lighting** before loading VanaView. The two plugins are incompatible.

> **Do not use the `drawdistance` addon with VanaView.** Use VanaView alone to control draw distance. Running both can cause conflicting settings, addon load errors or a client crash. Remove `drawdistance` from your startup script and unload it with `/addon unload drawdistance` before loading VanaView. The separate `distance` addon, which displays the distance to your target, is unaffected.

### Drawdistance compatibility

VanaView 2.11 blocks `/drawdistance` commands and attempts to load or reload that addon while VanaView is loaded. This applies in both **Game** and **Distance** fog modes. Use `/vv` to adjust draw distance instead.

If `drawdistance` was already loaded, its commands are still blocked, but VanaView does not unload it automatically or undo changes it made beforehand. Run `/addon unload drawdistance` and remove it from your startup script. If you already changed distances with the addon in the current session, restart the game with only VanaView controlling draw distance so those earlier values are not carried over.

The separate `distance` addon remains compatible. VanaView's draw distance, object range and fog controls continue to work normally.

## Install or update

1. Download the `VanaView-<version>.zip` asset from the [latest release](https://github.com/By-T0r/VanaView/releases/latest).
2. If VanaView is loaded, run `/unload vanaview`. Extract the ZIP into your Ashita folder. It already contains `plugins/VanaView.dll`.
3. Run `/load vanaview`, then `/vv` to open the settings in game.

## What VanaView changes

### Visibility and map detail

| Setting | Default | What it changes |
| --- | --- | --- |
| **Draw distance** | 20 | Sets how far the client draws the world and entities it already has, such as players and monsters. The slider runs from 1 to 40 in whole steps. |
| **Object range** | 1× | Extends the distance at which map objects such as trees and bushes appear. You can adjust it independently of **Draw distance**, from 1× to 4× in 0.25× steps. |
| **Map LoD** | Full | **Full** uses the most detailed model available for visible map objects, reducing sudden changes as you approach. **Game** switches between models at the usual distances. |
| **Extend distant terrain** | On | On compatible maps, helps keep distant terrain in view as you turn the camera. For example, it can keep the mountains northwest of the Crag of Dem in view from more angles at Konschtat Highlands (H-6). |

Higher **Draw distance** and **Object range** can lower FPS, especially in areas with dense scenery.

### Texture shimmer

VanaView generates missing mipmaps from supported game textures. Mipmaps are smaller versions of a texture that help reduce flicker as the camera moves. Both controls start on and can be adjusted independently.

- **Ground mipmaps** covers terrain and other supported scenery without cutout transparency.
- **Foliage mipmaps** covers supported scenery with cutout transparency, such as grass, leaves and flowers. The generated mipmaps help retain fine details like thin leaves and petals.

### Fog and character shadows

| Setting | Default | What it changes |
| --- | --- | --- |
| **Fog** | Game | **Game** keeps the usual fog distance for the current weather as you extend your view. **Distance** scales the fog with **Draw distance**. |
| **Simple actor shadows in Normal** | On | Gives characters, including your own, simple ground shadows when the game’s **Shadows** setting is **Normal**. |
| **Terrain shadow range** | Full | In **Normal**, **Full** lets shadows follow the ground even on distant slopes. **Native** uses the game’s usual terrain-following range. |

Set **Shadows** in the game to **High** for detailed silhouettes or **Off** to disable character shadows.

## Per-zone settings

VanaView uses one global profile by default. In `/vv`, expand **Zone settings** and enable **Use zone profiles** to save **Draw distance**, **Map LoD** and **Fog** for individual zones. Changes in the menu are saved automatically. Zones without a saved profile use the global settings.

**Object range**, mipmaps and shadows apply across zones. To turn off terrain extension in the current zone, use **Disable terrain extension here**. This switch works with either profile mode. VanaView stores preferences in `config/plugins/VanaView.ini`.

## In-game examples

### East Ronfaure

Look along the bend in the path to see the difference in tree detail and the amount of undergrowth drawn. Both screenshots use **Game** fog.

| 1× Object range · Game LoD | 2× Object range · Full LoD |
| --- | --- |
| [![Forest path with fewer trees around the bend](https://gist.githubusercontent.com/By-T0r/eec3906fc83c1fe9d019cf0ec0b0d1e4/raw/4724ec66858db776447a3146e3b7adc154ea3829/forest-game-preview.png)](https://gist.githubusercontent.com/By-T0r/eec3906fc83c1fe9d019cf0ec0b0d1e4/raw/25697a93ea4f09fb67d6dd86b4ef254f74ec77f2/forest-game.png) | [![Additional trunks and undergrowth along the same path](https://gist.githubusercontent.com/By-T0r/eec3906fc83c1fe9d019cf0ec0b0d1e4/raw/38cc28e8c711c63ed645bbf5ea8d1b732cb4086f/forest-full-preview.png)](https://gist.githubusercontent.com/By-T0r/eec3906fc83c1fe9d019cf0ec0b0d1e4/raw/f00d4e9693fd01dd13814ff4f270861ac9f495ef/forest-full.png) |
| Ground and foliage mipmaps off | Ground and foliage mipmaps on |

### La Theine Plateau

Look at the orange trees on the left and the ridge beyond the fence. With **Game** fog, the trees fade into the mist. **Draw distance** is 20 in both screenshots. The presets also differ in object range, model detail and mipmaps.

| Distance fog | Game fog |
| --- | --- |
| [![Orange trees and a distant ridge visible through the rain](https://gist.githubusercontent.com/By-T0r/eec3906fc83c1fe9d019cf0ec0b0d1e4/raw/41dc9b9823621ce0a5f66515b8164b0402d21212/plateau-distance-preview.png)](https://gist.githubusercontent.com/By-T0r/eec3906fc83c1fe9d019cf0ec0b0d1e4/raw/995e5054569452c03f96c9d6b2aaa1ee87ccb076/plateau-distance.png) | [![The same tree line fading into the mist](https://gist.githubusercontent.com/By-T0r/eec3906fc83c1fe9d019cf0ec0b0d1e4/raw/22d23628b467d413c715fcf7a6a92cdc4244ffd1/plateau-game-preview.png)](https://gist.githubusercontent.com/By-T0r/eec3906fc83c1fe9d019cf0ec0b0d1e4/raw/0937c2c989d11ec3468352ca369987f38e195481/plateau-game.png) |
| 1× Object range, Game LoD, mipmaps off | 2× Object range, Full LoD, mipmaps on |

### Terrain Extension

| Valkurm Dunes |
| --- |
| [![Distant ridgeline visible across the horizon in Valkurm Dunes](https://gist.githubusercontent.com/By-T0r/eec3906fc83c1fe9d019cf0ec0b0d1e4/raw/a12f513e4bf6edfa8991aa6f77de9e452f093a66/valkurm-dunes-terrain-extension-preview.png)](https://gist.githubusercontent.com/By-T0r/eec3906fc83c1fe9d019cf0ec0b0d1e4/raw/e45f6163d313b620684a9bca0c4158770a6447cc/valkurm-dunes-terrain-extension.png) |
| Terrain extension enabled. The distant ridgeline stays visible across the horizon. |

## Commands and troubleshooting

- Run `/vv help` for the command list. Use `/vv mip off` or `/vv mip on` to switch both mipmap settings together. Use `/vv coverage` to highlight scenery using VanaView mipmaps in green and `/vv coverage off` to clear the overlay.
- With dgVoodoo, set **DirectX → Mipmapping** to **App driven**. Trilinear and 8× anisotropic filtering have been tested. VanaView reuses generated ground mipmaps from a local disk cache capped at 64 MiB.
- **Restore defaults** in `/vv` asks for confirmation before resetting settings and turning off zone profiles. Press **Esc** or choose **Cancel** to keep your settings. The chat command `/vv reset` applies the defaults immediately.

### Reporting a stutter

Open `/vv` and click **Save diagnostic report** soon after the stutter. The chat message gives you the filename under `logs/VanaView` in your Ashita folder. The report contains up to 900 recent frame intervals, zone IDs, graphics settings and mipmap cache counters. You can review the local text file and attach it to an [issue](https://github.com/By-T0r/VanaView/issues) with a short description of what you were doing in game.

## Rendering scope

VanaView changes rendering in the local FFXI client. It uses map geometry, textures, weather and entities already available to the client. Networking, player actions, targeting and interaction are handled by the game.

The DLL adjusts graphics values in client memory, uses the map’s spatial tree for terrain extension, and applies mipmaps to eligible scenery through Direct3D 8 draw callbacks. It reads the current zone ID to select settings. The plugin entry point, commands and rendering callbacks are in [VanaView.cpp](src/VanaView.cpp).

## Build and verify

<details>
<summary>Build requirements and release verification</summary>

Set `ASHITA_SDK` to the directory containing `Ashita.h` for plugin interface 4.30. With the Visual Studio 2022 x86 C++ build tools installed, run `./build.ps1` to build `dist/VanaView.dll` and check its exports and plugin interface.

Run `./build.ps1 -Test` for command, configuration and rendering regressions. The optional arguments are `-D3D8` for an x86 D3D8 wrapper, `-TerrainFixtures` for pavement DDS files, `-FogImage` for a mapped client image and `-ShadowImages` for mapped client images supplied as a PowerShell array. For full foliage mip-chain comparisons, set `VV_FOLIAGE_FIXTURES` to a directory of `*-source.dds` files and `VV_FOLIAGE_EXPECTED` to a separate directory with the expected chains under the same filenames. The optional test fixtures are supplied separately.

Run `./tools/Verify-Release.ps1 -Destination ../vanaview-release-check` to rebuild and test a committed candidate in a fresh checkout. The script accepts the optional fixture arguments, compares the rebuilt DLL with the committed DLL and prints the SHA-256.

In game, cycle **Normal → High → Off → Normal**, then compare **Native** and **Full** terrain shadow range on a slope. Make a zone round trip with ground mipmaps enabled, save a diagnostic report and open the file named in chat. Open **Restore defaults**, press **Esc** and verify your settings. Tag the verified commit and use its `dist/VanaView.dll` for the release.

</details>

## Credits

**rockerudon** created the original VanaView plugin. **By-T0r** develops and maintains this fork.

Released under the [MIT License](LICENSE).
