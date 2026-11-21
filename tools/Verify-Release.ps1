# Rebuild the committed candidate in a new checkout; keep it for inspection.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Destination,
    [string]$Revision = 'HEAD',
    [string]$D3D8,
    [string]$TerrainFixtures,
    [string]$FogImage,
    [string[]]$ShadowImages
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$commit = git -C $repo rev-parse --verify --end-of-options "${Revision}^{commit}"
if ($LASTEXITCODE) { throw "Unknown commit: $Revision" }
if (Test-Path -LiteralPath $Destination) { throw "Choose a new checkout directory: $Destination" }
$options = @{}
foreach ($key in 'D3D8','TerrainFixtures','FogImage') {
    if ($PSBoundParameters[$key]) {
        $options[$key] = (Resolve-Path -LiteralPath $PSBoundParameters[$key]).Path
    }
}
if ($ShadowImages) { $options.ShadowImages = @($ShadowImages | ForEach-Object { (Resolve-Path -LiteralPath $_).Path }) }
git clone --local --no-checkout -- $repo $Destination
if ($LASTEXITCODE) { throw 'Could not clone the candidate.' }
Push-Location -LiteralPath $Destination
try {
    git checkout --detach $commit
    if ($LASTEXITCODE) { throw "Could not check out $commit" }
    $dll = 'dist/VanaView.dll'
    $expected = (Get-FileHash $dll).Hash
    & ./build.ps1 -Test @options
    if ((Get-FileHash $dll).Hash -ne $expected) { throw 'Rebuilt release DLL differs from the committed DLL.' }
    $changes = git status --porcelain
    if ($LASTEXITCODE -or $changes) { throw "Checkout changed during verification: $changes" }
    Write-Host "Verified commit $commit"
    Write-Host "Release SHA-256 $expected"
    Write-Host "Release DLL: $((Resolve-Path $dll).Path)"
} finally {
    Pop-Location
}
