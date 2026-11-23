<#
.SYNOPSIS
  Builds, tests and packages a LightTranslate release into .\dist:
    LightTranslate-<version>-setup.exe          per-user installer (Inno Setup)
    LightTranslate-<version>-portable-x64.zip   the bare exe + README
    SHA256SUMS.txt
  The version comes from project(VERSION) in CMakeLists.txt.

  Requirements: Visual Studio 2022+ with the C++ workload, Inno Setup 6
  (winget install JRSoftware.InnoSetup). Works from a plain PowerShell window;
  the MSVC environment is loaded automatically.
#>
param(
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Invoke-Native([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe $($Arguments -join ' ') failed with exit code $LASTEXITCODE" }
}

# ---- Version ------------------------------------------------------------------
$cmake = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(LightTranslate VERSION (\d+\.\d+\.\d+)') { throw 'No project(LightTranslate VERSION x.y.z) in CMakeLists.txt' }
$version = $Matches[1]
if ($env:GITHUB_REF_TYPE -eq 'tag' -and $env:GITHUB_REF_NAME -ne "v$version") {
    throw "Tag $env:GITHUB_REF_NAME does not match the CMake version $version (expected v$version)"
}
Write-Host "LightTranslate $version" -ForegroundColor Cyan

# ---- MSVC environment ---------------------------------------------------------
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio with the C++ workload was not found' }
    $script = Join-Path $env:TEMP 'lighttranslate-vcvars.cmd'
    Set-Content $script "@call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1`r`n@set" -Encoding ascii
    foreach ($line in (cmd /c $script)) {
        if ($line -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] }
    }
    Remove-Item $script
}

# ---- Build + test -------------------------------------------------------------
Invoke-Native cmake @('--preset', 'dist')
Invoke-Native cmake @('--build', '--preset', 'dist')
if (-not $SkipTests) {
    Invoke-Native ctest @('--preset', 'dist')
}
$exe = Join-Path $root 'build\dist\LightTranslate.exe'

# ---- Package ------------------------------------------------------------------
$dist = Join-Path $root 'dist'
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory $dist | Out-Null

$iscc = @(
    (Get-Command iscc.exe -ErrorAction SilentlyContinue).Source,
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 not found (winget install JRSoftware.InnoSetup)' }
Invoke-Native $iscc @('/Qp', "/DAppVersion=$version", "/DSourceExe=$exe", "/DOutputDir=$dist",
                      (Join-Path $root 'installer\LightTranslate.iss'))

$staging = Join-Path $env:TEMP "LightTranslate-$version-portable"
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
New-Item -ItemType Directory $staging | Out-Null
Copy-Item $exe, (Join-Path $root 'README.md') $staging
Compress-Archive -Path "$staging\*" -DestinationPath (Join-Path $dist "LightTranslate-$version-portable-x64.zip")
Remove-Item $staging -Recurse -Force

$sums = Get-ChildItem $dist -File | ForEach-Object { "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name }
Set-Content (Join-Path $dist 'SHA256SUMS.txt') $sums -Encoding ascii

Write-Host "`nRelease files:" -ForegroundColor Green
Get-ChildItem $dist | Format-Table Name, @{ n = 'Size (KB)'; e = { [math]::Round($_.Length / 1KB) } } -AutoSize
