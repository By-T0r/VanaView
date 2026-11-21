# Build script for the VanaView Ashita plugin.
#
# Requirements:
#   - Visual Studio 2022 with the C++ x86 (32-bit) build tools.
#   - The Ashita v4 plugin SDK headers.
#
# Set ASHITA_SDK to the Ashita plugin SDK folder that contains Ashita.h.
# FFXI is a 32-bit process, so this builds x86.

param(
    [switch]$Test,
    [string]$D3D8,
    [string]$TerrainFixtures,
    [string]$FogImage,
    [string[]]$ShadowImages
)
$ErrorActionPreference = 'Stop'

$root     = $PSScriptRoot
$src      = Join-Path $root 'src\VanaView.cpp'
$buildDir = Join-Path $root 'build'
$distDir  = Join-Path $root 'dist'
$outDll   = Join-Path $distDir 'VanaView.dll'
$obj      = Join-Path $buildDir 'VanaView.obj'

$sdk = $env:ASHITA_SDK
if ([string]::IsNullOrWhiteSpace($sdk) -or -not (Test-Path (Join-Path $sdk 'Ashita.h'))) {
    throw 'Ashita SDK not found. Set ASHITA_SDK to the folder that contains Ashita.h.'
}

$vcvarsCandidates = @(
    'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat'
)

$vcvars = $vcvarsCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) {
    throw 'Could not find Visual Studio vcvars32.bat. Install the Visual Studio C++ x86 build tools.'
}

New-Item -ItemType Directory -Force $buildDir | Out-Null
New-Item -ItemType Directory -Force $distDir  | Out-Null

$clArgs = @(
    '/nologo',
    '/std:c++20',
    '/EHa',
    '/O2',
    '/MT',
    '/LD',
    '/W3',
    '/D_CRT_SECURE_NO_WARNINGS',
    "/I`"$sdk`"",
    "/Fe:`"$outDll`"",
    "/Fo:`"$obj`"",
    "`"$src`""
    '/link',
    '/Brepro',
    '/NODEFAULTLIB:libci.lib',
    '/NODEFAULTLIB:libc.lib',
    'legacy_stdio_definitions.lib',
    'advapi32.lib',
    'gdi32.lib',
    "`"$sdk/d3d8/lib/d3dx8.lib`"",
    "/IMPLIB:`"$buildDir\VanaView.lib`""
) -join ' '

$command = "`"$vcvars`" >nul && cl.exe $clArgs"
cmd.exe /c $command
if ($LASTEXITCODE -ne 0) {
    throw "VanaView build failed with exit code $LASTEXITCODE."
}

& (Join-Path $root 'tools\Test-Plugin.ps1') -PluginPath $outDll

Write-Host "Built Release plugin: $outDll"
Write-Host 'Copy it to Ashita/plugins/ and load it in game with: /load vanaview'

if ($Test) {
    $names = @('Test-MipCommands', 'Test-Rendering')
    if ($FogImage) { $names += 'Test-FogScale' }
    if ($ShadowImages) { $names += 'Test-NormalShadows' }
    foreach ($name in $names) {
        $source = Join-Path $root "tools/$name.cpp"
        $exe = Join-Path $buildDir "$name.exe"
        $object = Join-Path $buildDir "$name.obj"
        $argsCl = "/nologo /std:c++20 /EHa /O2 /MT /D_CRT_SECURE_NO_WARNINGS /I`"$sdk`" /Fo:`"$object`" /Fe:`"$exe`" `"$source`" /link /NODEFAULTLIB:libci.lib /NODEFAULTLIB:libc.lib user32.lib advapi32.lib gdi32.lib legacy_stdio_definitions.lib `"$sdk/d3d8/lib/d3dx8.lib`""
        cmd.exe /c "`"$vcvars`" >nul && cl.exe $argsCl"
        if ($LASTEXITCODE) { throw "Compilation failed for $name" }
        $arguments = @()
        if ($name -eq 'Test-Rendering') {
            $arguments += $buildDir
            if ($D3D8) { $arguments += $D3D8 }
            elseif ($TerrainFixtures) { $arguments += 'd3d8.dll' }
            if ($TerrainFixtures) { $arguments += $TerrainFixtures }
        } elseif ($name -eq 'Test-FogScale') {
            $arguments += $FogImage
        }
        if ($name -eq 'Test-NormalShadows') {
            foreach ($image in $ShadowImages) {
                & $exe $image
                if ($LASTEXITCODE) { throw "$name failed for $image" }
            }
            continue
        }
        & $exe @arguments
        if ($LASTEXITCODE) { throw "$name failed ($LASTEXITCODE)" }
    }
}
