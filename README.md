# VanaView

An [Ashita v4](https://www.ashitaxi.com/) plugin for **Final Fantasy XI**.

VanaView adds an ImGui window for tuning the client's visual distance, map
level-of-detail behavior, and fog range. It is meant to reduce world pop-in and
make distant map props such as trees, flowers, and terrain details appear more
consistently.

This is a **client-side visual plugin only**. It does not change movement speed,
combat, cooldowns, NPC behavior, server state, or how other players see you.

## Features

- ImGui configuration window with a neutral dark theme.
- Smooth `Distance` slider for world/entity draw distance.
- `Map LoD` controls:
  - `Range`: extends the distance before map objects switch to lower LoD.
  - `Full`: forces the highest available map LoD where supported.
- Optional custom fog range, useful when increasing draw distance but still
  wanting distant fog to remain visible.
- Per-zone profiles, so each zone can keep its own distance, LoD, and fog
  settings.
- Restores the original client values when unloaded or when pressing
  `Default`.

## Install

1. Download `VanaView.dll` from the [latest release](../../releases) (or the
   `dist/` folder).
2. Copy it into your Ashita plugins folder:

   ```text
   Ashita/plugins/VanaView.dll
   ```

3. Load it in game:

   ```text
   /load vanaview
   ```

4. Open the window:

   ```text
   /vv
   ```

## Commands

| Command | Description |
| --- | --- |
| `/vv` or `/vanaview` | Toggle the configuration window. |
| `/vv status` | Show the current distance, LoD, and fog state. |
| `/vv distance <1-100>` | Set draw distance using the same smooth scale as the UI slider. |
| `/vv distance default` | Restore the original draw distance captured from the client. |
| `/vv lod <scale>` | Enable LoD range mode. `1.00` is the normal game distance; higher values extend it. |
| `/vv lod full` | Force highest map LoD where the current client code path supports it. |
| `/vv lod default` | Restore the game's normal LoD behavior. |
| `/vv fog <start> <end>` | Enable custom fog range. |
| `/vv fog default` | Restore normal game fog behavior. |
| `/vv zone on` | Enable per-zone profiles. |
| `/vv zone off` | Disable per-zone profiles and return to the global profile. |
| `/vv zone status` | Show whether per-zone profiles are enabled and the current zone id. |
| `/vv reset` | Restore the current profile to game defaults. |

## Configuration

Settings are saved to:

```text
Ashita/config/plugins/VanaView.ini
```

By default, VanaView uses one global profile. If `Use zone profiles` is enabled,
the plugin saves each zone under its own INI section, such as:

```text
[zone.101]
distance=...
lod_mode=...
lod_range=...
fog_mode=...
fog_start=...
fog_end=...
```

When changing zones, VanaView saves the previous zone profile and loads the new
zone profile automatically. If a zone does not have a saved profile yet, it
starts from the current/global settings.

## Notes

Increasing distance and LoD can make the game render more objects and higher
detail meshes, which may lower FPS depending on the zone and hardware.

Custom fog range intentionally overrides the game's own fog start/end values.
Leave it disabled if you want the game's normal day/night or zone-specific fog
changes to happen naturally.

VanaView does not send packets and does not interact with server-side systems.

## Building from source

Requires the **Visual Studio 2022 C++ x86 build tools**. FFXI is a 32-bit
process, so the plugin must be built as x86.

1. Make sure the Ashita plugin SDK headers are available and update the `$sdk`
   path at the top of `build.ps1` to point at your `Ashita/plugins/sdk` folder.
2. Run:

   ```powershell
   powershell -ExecutionPolicy Bypass -File build.ps1
   ```

   This produces `dist/VanaView.dll`.

## License

Released under the MIT License. See [LICENSE](LICENSE).

## Credits

Created by **rockerudon**.

The draw distance control is based on the same client values used by the Ashita
`drawdistance` addon.

## Support

If you find this project useful, you can support development here:

<a href="https://www.buymeacoffee.com/rockmizx" target="_blank"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me A Coffee" height="50" width="210"></a>
