# VanaView

An Ashita v4 plugin for Final Fantasy XI. Use `/vv` to adjust draw distance,
map LoD, fog and terrain filtering.

## Install and use

1. Download [VanaView.dll](dist/VanaView.dll) for Ashita plugin interface 4.30.
2. Copy it to the Ashita `plugins` directory and run `/load vanaview`.
3. Open `/vv` and use **Terrain mipmaps** to change the global setting. It
   defaults to on, and changes are saved automatically.

Terrain mipmaps work with Bump Mapping enabled or disabled. If you use
dgVoodoo, set **DirectX → Mipmapping** to **App driven**. Trilinear and
Anisotropic 8× filtering were both tested. Use `/vv mip status` to check
substitutions and cache activity.

**Phoenix Lighting is incompatible with VanaView.** Disable it before loading
VanaView.

## Build and contribute

Use the Visual Studio 2022 x86 C++ build tools and an Ashita SDK that targets
plugin interface 4.30. Set `ASHITA_SDK` to the SDK directory containing
`Ashita.h`, then run:

```powershell
.\build.ps1
```

The build writes `dist/VanaView.dll` and runs an x86 load and export check.
The plugin entry point is in `src/VanaView.cpp`. Fog and mipmap logic live in
their own headers under `src/`. The Direct3D regression harness is in
`tools/Test-RenderDiagnostics.cpp`.

Created by **rockerudon**. Released under the [MIT License](LICENSE).
