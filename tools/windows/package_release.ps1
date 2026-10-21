# Build the public release zip: this game's native build, WITHOUT any game files.
#   powershell -ExecutionPolicy Bypass -File tools\windows\package_release.ps1
# The version comes from CMakeLists.txt (project VERSION), the same one the game shows in its menu.
# Output: dist\v<version>\SVR2008-NATIVE.zip (and the unpacked folder dist\SVR2008-NATIVE-v<version>).
# Players put their own disc image next to svr2008.exe (or pick it on first start) and run it.
param([string]$Version = "")
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
$ProjectVersion = [regex]::Match((Get-Content "$Root\CMakeLists.txt" -Raw), "project\(\S+ VERSION ([0-9.]+)").Groups[1].Value
if (-not $Version) { $Version = $ProjectVersion }
if ($Version -ne $ProjectVersion) { throw "-Version $Version differs from CMakeLists.txt ($ProjectVersion): the game would show the wrong version" }
$Id = "svr2008"
$Title = "WWE SmackDown vs. Raw 2008"
$Name = "SVR2008-NATIVE"
$Build = "$Root\out\build\win-amd64-release"
if (-not (Test-Path "$Build\$Id.exe")) { throw "Build the game first: tools\windows\build.ps1" }
# A CMake cache reset once silently rebuilt with the emulated GPU: only the native build ships.
if (-not (Select-String -Path "$Build\CMakeCache.txt" -Pattern "^SVR_NATIVE_RENDERER:BOOL=ON" -Quiet)) {
  throw "$Build is not a native-renderer build"
}
$BuildD3D12 = "$Build-d3d12"
if (-not (Test-Path "$BuildD3D12\$Id.exe")) { throw "Build the Direct3D 12 program first: tools\windows\build.ps1" }
if (-not (Select-String -Path "$BuildD3D12\CMakeCache.txt" -Pattern "^SVR_D3D12:BOOL=ON" -Quiet)) {
  throw "$BuildD3D12 is not a Direct3D 12 build"
}

$Out = "$Root\dist\$Name-v$Version"
if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
New-Item -ItemType Directory -Force $Out, "$Out\licenses" | Out-Null
# Two programs (src/graphics_api.h): $Id.exe draws with Direct3D 12, and starts $Id`_vulkan.exe
# instead under Proton (Steam Deck, Linux) or when the Graphics API setting says Vulkan.
Copy-Item "$BuildD3D12\$Id.exe" "$Out\$Id.exe"
Copy-Item "$Build\$Id.exe" "$Out\$($Id)_vulkan.exe"
foreach ($f in "rexruntime.dll", "rexgpu-xenos.dll") { Copy-Item "$Build\$f" $Out }
# Microsoft's shader compiler, for any Direct3D 12 shader variant without a prebuilt copy.
foreach ($f in "dxcompiler.dll", "dxil.dll") { Copy-Item "$BuildD3D12\$f" $Out }
Copy-Item "$Build\fonts" $Out -Recurse
# Graphics pipelines the game uses (render states and shader hashes, no game data), recorded by
# playing the build: precompiled in the background at startup instead of hitching mid-match.
if (Test-Path "$Build\pipelines.bin") { Copy-Item "$Build\pipelines.bin" $Out }
# Visual C++ runtime, app-local (Proton and PCs without the redistributable).
foreach ($f in "msvcp140.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll") {
  Copy-Item "$env:WINDIR\System32\$f" $Out
}

@"
# $Title settings. Edit with any text editor; command-line arguments override these.
fullscreen = true
svr_60fps = true
# Keyboard controls are not tested yet; a controller is recommended (rebind keys with F4 in game).
mnk_mode = true
# Internal resolution: 0 = auto (1440p on 1080p/1440p screens, 4K on 4K screens, 720p on
# Steam Deck), 1 = 720p (original), 2 = 1440p, 3 = 4K, 6 = 8K, or a height: 480, 1080.
svr_render_scale = 0
# Screen shape: 5 = the game's 16:9 (thin bars on 16:10 screens such as the Steam Deck),
# 6 = stretch to fill.
bd_aspect_ratio = 5
log_file = "game.log"
"@ | Set-Content "$Out\$Id.toml" -Encoding ascii

@"
$Title - native PC version (Windows; Linux and Steam Deck through Proton)

YOU NEED YOUR OWN COPY OF THE GAME. This download contains none of the files from the game disc
and never downloads any: the game's data (models, textures, sound, video) is read from a disc
image (.iso) of your own disc, USA / Europe release. It does not condone piracy.

Windows
 1. Copy your .iso into this folder, next to $Id.exe.
 2. Run $Id.exe. The game finds the disc image by itself and starts.
 (If there's no .iso here, the game asks for one the first time and remembers it.)

Linux / Steam Deck, through Proton
 1. Extract this folder (on the Deck in Desktop Mode, for example to /home/deck/Games/$Name)
    and copy your .iso into it, next to $Id.exe. The game finds it there by itself.
 2. Steam > Games > Add a Non-Steam Game to My Library > Browse > pick $Id.exe
    (set the file type filter to All files).
 3. The shortcut's Properties > Compatibility > Force the use of a specific Steam Play
    compatibility tool > Proton Experimental.
 4. Start it (on the Deck, from Game Mode). View + Menu together opens the settings menu.
    Optional: bind the L4 back button to F1 in the game's Steam controller layout.

Settings: press F1 in game (or Back + Start on a controller) for the settings menu: resolution
from 480p up to 8K, graphics API, fullscreen or window, 60 or 30 fps, screen shape, edge blur,
background blur, FPS counter, sound,
language (English, Spanish, German, Italian), keyboard controls, CPU priority. Saved in $Id.toml.
Graphics: $Id.exe draws with Direct3D 12 on Windows; on Steam Deck / Linux it starts
$($Id)_vulkan.exe (Vulkan) by itself. Settings > Graphics API switches between them.
Saves: the userdata folder (created on first start). If something goes wrong, send game.log.
Updates: when a new version is out, the game says so at startup; install it from the settings
menu (Update), only if you want to. Your saves, settings and disc image are kept, and your saves
are backed up to the save_backups folder first. Turn the check off there too.

Licence: this program is free software under the GNU General Public License v3.0
(licenses/this project (GPL-3.0).txt); the other licences are in the licenses folder.
Source code: https://github.com/nefariousjosiah/WWE-SVR2008-RECOMP
"@ | Set-Content "$Out\README.txt" -Encoding ascii

# Licence notices that travel with the binaries.
$Notices = @{
  "LICENSE" = "this project (GPL-3.0).txt"; "THIRD_PARTY.md" = "THIRD_PARTY.md"
  "src\reblue\LICENSE.reblue" = "re-Blue renderer (BSD-3-Clause).txt"
  "third_party\rexglue-sdk\LICENSE" = "ReXGlue SDK (BSD-3-Clause).txt"
  "third_party\plume\LICENSE" = "plume (MIT).txt"
  "third_party\reblue_thirdparty\zstd\LICENSE" = "zstd (BSD).txt"
  "res\fonts\OFL.txt" = "Press Start 2P font (OFL).txt"
  "third_party\rexglue-sdk\thirdparty\sdl3\LICENSE.txt" = "SDL3 (zlib).txt"
  "third_party\rexglue-sdk\thirdparty\imgui\LICENSE.txt" = "Dear ImGui (MIT).txt"
  "res\fonts\LICENSE-Roboto.txt" = "Roboto font (Apache-2.0).txt"
  # Libraries inside rexruntime.dll / rexgpu-xenos.dll (THIRD_PARTY.md, "Built into the game's DLLs").
  "third_party\rexglue-sdk\thirdparty\FFmpeg\COPYING.LGPLv2.1" = "FFmpeg (LGPL-2.1).txt"
  "third_party\rexglue-sdk\thirdparty\libmspack\libmspack\COPYING.LIB" = "libmspack (LGPL-2.1).txt"
  "third_party\rexglue-sdk\thirdparty\fmt\LICENSE" = "fmt (MIT).txt"
  "third_party\rexglue-sdk\thirdparty\spdlog\LICENSE" = "spdlog (MIT).txt"
  "third_party\rexglue-sdk\thirdparty\tomlplusplus\LICENSE" = "toml++ (MIT).txt"
  "third_party\rexglue-sdk\thirdparty\xxHash\LICENSE" = "xxHash (BSD-2-Clause).txt"
  "third_party\rexglue-sdk\thirdparty\utfcpp\LICENSE" = "utf8-cpp (BSL-1.0).txt"
  "third_party\licenses\disruptorplus-LICENSE.txt" = "disruptorplus (MIT).txt"
  "third_party\rexglue-sdk\thirdparty\o1heap\LICENSE" = "o1heap (MIT).txt"
  "third_party\rexglue-sdk\thirdparty\aes_128\LICENSE" = "aes_128 (MIT).txt"
  "third_party\rexglue-sdk\thirdparty\simde\COPYING" = "SIMDe (MIT).txt"
  "third_party\rexglue-sdk\thirdparty\glslang\LICENSE.txt" = "glslang.txt"
  "third_party\rexglue-sdk\thirdparty\vulkan-memory-allocator\LICENSE.txt" = "Vulkan Memory Allocator (MIT).txt"
  "third_party\rexglue-sdk\thirdparty\vulkan-headers\LICENSE.md" = "Vulkan headers.txt"
  "third_party\rexglue-sdk\thirdparty\spirv-headers\LICENSE" = "SPIR-V headers.txt"
  "third_party\licenses\renderdoc_app-LICENSE.txt" = "RenderDoc API header (MIT).txt"
  # The updater (src/updater.cpp).
  "third_party\rexglue-sdk\thirdparty\inja\third_party\include\nlohmann\LICENSE.MIT" = "nlohmann json (MIT).txt"
  "third_party\licenses\stb-LICENSE.txt" = "stb (MIT or public domain).txt"
  # Direct3D 12: Microsoft's shader compiler (dxcompiler.dll) and the linked-shader cache (miniz).
  "third_party\licenses\DirectXShaderCompiler-LICENSE.txt" = "DirectX Shader Compiler (NCSA).txt"
  "third_party\reblue_thirdparty\miniz\LICENSE" = "miniz (public domain).txt"
}
foreach ($k in $Notices.Keys) { Copy-Item "$Root\$k" "$Out\licenses\$($Notices[$k])" }

$Zip = "$Root\dist\v$Version\$Name.zip"
New-Item -ItemType Directory -Force (Split-Path $Zip) | Out-Null
if (Test-Path $Zip) { Remove-Item $Zip -Force }
# Entry paths need forward slashes: Windows PowerShell's Compress-Archive writes backslashes, which
# Linux and the Steam Deck read as part of the file name ("fonts\... doesn't exist").
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$Archive = [IO.Compression.ZipFile]::Open($Zip, [IO.Compression.ZipArchiveMode]::Create)
try {
  foreach ($f in Get-ChildItem $Out -Recurse -File) {
    $Entry = $f.FullName.Substring($Out.Length + 1).Replace("\", "/")
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($Archive, $f.FullName, $Entry,
      [IO.Compression.CompressionLevel]::Optimal)
  }
} finally { $Archive.Dispose() }
Write-Host ("Release: {0} ({1:N0} MB)" -f $Zip, ((Get-Item $Zip).Length / 1MB))
