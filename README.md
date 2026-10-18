# WWE SmackDown vs. Raw 2008 — native PC, Linux & Steam Deck

The Xbox 360 version of **WWE SmackDown vs. Raw 2008**, running natively on Windows, and on Linux
and the Steam Deck through Proton: no emulator. The game's own program is recompiled to run
directly on your PC, and its graphics go through a native renderer (Direct3D 12 on Windows,
Vulkan on Linux and the Steam Deck), at 60 fps and up to 4K.

**The goal is simple: bring the game back so people can play it again, natively on PC at 60 fps.**
It's the same game you remember, with no mods, roster changes or reworked gameplay. The only changes
are the ones it needs to run well on today's hardware: a native renderer, a steady 60 fps (using the
game's own 60 fps mode, so everything plays at the right speed) and higher resolutions.

> **You need your own copy of the game.** None of the files from the game disc are in this project
> or its downloads, and nothing is ever downloaded for you: the game runs from a disc image
> (`.iso`) made from your own SvR 2008 disc. It does not condone piracy: please buy the game.

> **No installer, no launcher.** Put your `.iso` in the game's folder and run **`svr2008.exe`**. In
> game, **F1** (or **Back + Start** on a controller, **View + Menu** on the Steam Deck) opens the
> settings menu.

![The in-game settings menu (F1, or Back + Start on a controller)](docs/settings-menu.png)

## Showcase

<p align="center">
  <img src="docs/showcase/slam.gif" width="480" alt="Triple H lifts Randy Orton and drops him">
  <img src="docs/showcase/pedigree.gif" width="480" alt="Triple H hits the Pedigree on Randy Orton">
  <img src="docs/showcase/top-rope.gif" width="480" alt="Triple H comes off the top rope onto Randy Orton">
  <img src="docs/showcase/pin.gif" width="480" alt="Triple H pins Randy Orton for the three count">
</p>

## Features

- **Native, not emulated.** The game's PowerPC code is statically recompiled to x86-64
  ([ReXGlue](https://github.com/rexglue/rexglue-sdk)), and the game's own Direct3D calls are drawn
  by a native renderer (adapted from [re:Blue](https://github.com/zolaware/reblue)) with the
  game's shaders recompiled ahead of time. No GPU emulation.
- **Direct3D 12 on Windows, Vulkan on Linux and the Steam Deck.** `svr2008.exe` draws with
  Direct3D 12; under Proton it starts `svr2008_vulkan.exe` by itself, so Deck players change
  nothing. *Graphics API* in the settings menu switches between them (if one doesn't suit your
  graphics card, try the other).
- **Runs smoothly on lower-end PCs too.** The renderer does far less work per frame: the graphics
  driver's work runs on its own thread, and textures, geometry and shader settings are only sent
  again when they change. Every shader variant is prepared when the game is built, and the
  graphics pipelines the game uses are compiled in the background while each scene loads, instead
  of in the middle of an entrance. A GTX 1060 with an i7-8700 holds 60 fps. **CPU priority: High**
  in the settings menu helps further on busy or weak PCs.
- **60 fps** in menus and matches, using the game's own 60 fps mode so game speed stays right
  (the original 30 fps is one setting away).
- **Up to 4K.** Internal resolution follows your screen: 1440p on 1080p and 1440p monitors, 4K on
  4K monitors, 720p on the Steam Deck and on laptops' built-in graphics. The settings menu shows
  what Auto picked, or pick 720p / 1440p / 4K yourself. 16x anisotropic filtering.
- **Sharper image:** the game's own edge-blur filter (the console's cheap anti-aliasing, which
  smeared hair, tattoos, ropes and the crowd) is off; the higher internal resolution smooths edges
  instead. `svr_edge_blur = true` in `svr2008.toml` brings the original look back.
- **In-game settings menu:** F1, or Back + Start on a controller (Share + Options on PlayStation): resolution, graphics API,
  fullscreen or window, 60 or 30 fps, screen shape, FPS counter, sound, keyboard controls, how
  controllers map to players and CPU priority. Saved for next time.
- **Updates in the game, when you want them:** a new version is announced at startup and installed
  from the settings menu only if you choose to. Your saves are kept and backed up first (see
  [Updating](#updating)).
- **Linux and Steam Deck** through Proton, Steam's compatibility layer: put your disc image in the
  game's folder and run the game. On the Deck: 720p at 60 fps, the correct 16:9 shape (or
  stretched to fill).
- **Plays straight from your disc image**: nothing is extracted or installed.
- **Controllers** (Xbox, PlayStation, Switch Pro, the Deck's controls) through SDL. Keyboard controls
  are included but not tested yet, so a controller is recommended.

## What you need

- **Your own SvR 2008 disc image**, USA / Europe release (one release for both regions: title ID
  `5451080B`, media ID `5E35F037`, about 7.8 GB). Other releases aren't supported
  yet.
- **Windows 10 or 11, 64-bit**, and a graphics card with **Direct3D 12** or **Vulkan** support
  (AMD, NVIDIA or Intel, with a recent driver). Or **Linux** with Steam (Proton) and Vulkan
  drivers, such as a **Steam Deck**.
- About 130 MB for the program, plus your disc image.

## Windows: install and play

1. Download `SVR2008-NATIVE.zip` from the
   [Releases](https://github.com/nefariousjosiah/WWE-SVR2008-RECOMP/releases) page (under
   *Assets*; not *Source code*, which is the developer source).
2. Extract it to a folder of its own, for example `C:\Games\SVR2008-NATIVE`
   (not inside `Program Files`).
3. Copy your `.iso` into that folder, next to `svr2008.exe`.
4. Run **`svr2008.exe`**. The game finds the disc image by itself and starts.

Good to know:

- **Settings:** press **F1** in game (or **Back + Start** on a controller, **Share + Options** on PlayStation) for the settings menu:
  resolution (Auto, 720p, 1440p, 4K), graphics API (Auto, Direct3D 12, Vulkan), fullscreen or
  window, 60 or 30 fps, screen shape, FPS counter, sound, keyboard controls, **Controllers**
  (separate players, or every controller on player 1) and CPU priority. They are saved in `svr2008.toml`.
- **"Windows protected your PC"**: the program isn't code-signed. Click *More info* and then
  *Run anyway*.
- **Quitting:** close the game window (Alt+F4 or the close button).
- **Saves** live in the `userdata` folder. Back it up to keep your career and created superstars.
- **Updates:** from v0.2 on, the game can update itself when you choose to: see
  [Updating](#updating).
- **Disc image somewhere else?** If there's no `.iso` in the folder, the game asks for one the
  first time and remembers it.

### Controls

**A controller is recommended**; plug it in before starting. Each controller is its own player
(player 1, player 2...). Playing alone and the game says player 2 is disconnected, or your
controller stops responding? A controller that reconnects, or shows up twice through Steam Input /
DS4Windows / DSX, can become player 2: set **Controllers** to **All on player 1** in the settings
menu. Keyboard controls are currently not
tested yet. Their default layout (rebind with **F4** in game):

| Controller | Keyboard | | Controller | Keyboard |
|---|---|---|---|---|
| A | Space / ; | | Left stick | W A S D |
| B | Backspace / ' | | Right stick | Arrow keys |
| X | L | | D-pad | Shift + arrow keys |
| Y | P | | LB / RB | 1 / 3 |
| Start | Enter / X | | LT / RT | Q / E |
| Back | Tab / Z | | Stick presses | F / K |

**F1** (or **Back + Start**) opens the settings menu; **F2** shows or hides the FPS counter.

## Updating

The game can update itself, but only when you choose to:

- **At startup** it asks GitHub, in the background, whether a newer version of this port is out.
  If there is one, a notice shows for a few seconds. Nothing is downloaded yet.
- **To update,** open the settings menu (**F1**, or **Back + Start** on a controller), go down to
  **Update** and press **A** (or Enter): the menu shows what's new in that version. Press **A**
  once more to install it. It backs up your saves, downloads the new version, checks it and
  installs it. Then choose **Restart now**.
- **Your saves are kept.** The `userdata` folder (saves, profiles, created superstars) is never
  changed, and before every update your saves are also copied to `save_backups\<version>_<date>`
  in the game's folder. Your settings (`svr2008.toml`) and your disc image are kept too. If
  something goes wrong partway, the update stops and the version you had is put back.
- **Rather not?** Set **Check for updates** to *Off* in the settings menu and the game never
  goes online. The check only asks GitHub for this project's latest release; nothing about you or
  your PC is sent.
- **Coming from v0.1:** download the new zip once by hand (see
  [Windows: install and play](#windows-install-and-play)); from v0.2 on, the game does it.

## Linux and Steam Deck (Proton): install and play

On Linux the Windows release runs through Proton, Steam's compatibility layer; there's no separate
Linux build. The Steam Deck is the tested setup and holds 60 fps; other Linux PCs with Steam and
up-to-date Vulkan drivers should work the same way. With your disc image in the same folder as `svr2008.exe`, the
game finds it by itself.

The steps below are for the Steam Deck. On another Linux PC, do the same in Steam's desktop
client and skip the Desktop Mode and Game Mode parts.

**In Desktop Mode** (press the Steam button, *Power*, *Switch to Desktop*):

1. **Get the release.** Download `SVR2008-NATIVE.zip`
   (under *Assets*, not *Source code*) with a browser, or copy it from your PC together with your
   disc image. A USB stick for the copy must be **exFAT or NTFS**: the disc image is bigger than
   FAT32's 4 GB file limit.
2. **Extract it.** In the *Dolphin* file manager, make a folder such as
   `/home/deck/Games/SVR2008-NATIVE`, right-click the zip, *Extract*, *Extract archive to...*,
   and choose that folder. (A microSD card works too.)
3. **Add your disc image.** Copy your `.iso` into that folder, next to `svr2008.exe`.
4. **Add it to Steam.** Open Steam (desktop), then *Games* > *Add a Non-Steam Game to My
   Library...* > *Browse...*. Set the file type filter to *All files*, pick `svr2008.exe` in your
   folder, then *Add Selected Programs*.
5. **Turn on Proton.** In your library, right-click *svr2008* > *Properties...* >
   *Compatibility*, tick *Force the use of a specific Steam Play compatibility tool* and choose
   **Proton Experimental** (or the newest Proton). While you are there, rename the shortcut to
   *WWE SmackDown vs. Raw 2008*.

**Back in Game Mode** (the *Return to Gaming Mode* icon on the desktop):

6. Start it from *Library* > *Non-Steam*. The game starts fullscreen at 720p and 60 fps. To quit,
   press the Steam button and *Exit game*.

Notes for the Deck:

- **The first start** takes a little longer while Proton sets itself up.
- **Settings:** press **View + Menu** together (the two buttons left and right of the screen) to
  open the settings menu. To open it with the top-left back button instead: the game's
  *Controller settings* > *Edit Layout* > *Back Grip Buttons* > **L4** > *Keyboard* > **F1**.
- **Black bars:** the game is 16:9 and the Deck's screen is 16:10, so there are thin bars above
  and below. *Screen shape* > *Stretch to fill* in the settings menu fills the screen.
- **Controls:** the Deck's built-in controls work as an Xbox controller with Steam's default
  layout.
- **Artwork:** Steam's library artwork for the shortcut can be set from its properties.

## Troubleshooting

| What happens | What to do |
|---|---|
| A window asks for a disc image | There's no `.iso` next to `svr2008.exe`: pick your disc image (it's remembered), or copy it into the game's folder. |
| The game closes right after starting | Check that your disc image is the USA / Europe release of SvR 2008 and complete. |
| Black screen, or the game closes | Update your graphics driver, then try again. Still happening: **F1** > *Graphics API* > the other one (or add `svr_graphics_api = "vulkan"` to `svr2008.toml`), and start again. If it keeps happening, open an issue with `game.log` from the game's folder. |
| It runs slowly | Make sure you have the latest version (v1.0 made a big difference on older PCs). **F1** > *CPU priority* > **High** helps on a busy or weak PC. On a weak graphics card, **F1** > *Resolution* > **720p** (applies on the next start) needs less from it. If it still drops, open an issue with `game.log`, your processor and graphics card. |
| Windows blocks it | *More info* > *Run anyway* (the program isn't code-signed). |

## How it works

- **Recompilation.** [ReXGlue](https://github.com/rexglue/rexglue-sdk) translates the game's Xbox
  360 executable into C++ that is compiled for x86-64. The Xbox 360 system calls it makes are
  provided by the ReXGlue runtime, which also reads the game's data straight from your disc image.
- **Rendering.** Instead of emulating the Xbox 360 GPU, the renderer watches the game's own
  Direct3D library (statically linked into the game) and draws the same scenes through
  [plume](https://github.com/zolaware/plume) on Direct3D 12 or Vulkan (two builds of the game,
  `svr2008.exe` and `svr2008_vulkan.exe`). The game's shaders are converted ahead of time with
  [XenosRecomp](https://github.com/zolaware/reblue-XenosRecomp); for Direct3D 12 every variant is
  also linked ahead of time. The emulated GPU only keeps the game's command stream moving.
- **60 fps.** The game has its own 60 fps mode, which it normally drops to 30 for matches; a hook
  keeps it at 60, so the game logic and the animations stay in step.

## Building from source

For developers; players use the releases. Windows, with about 30 GB free.

```
git clone https://github.com/nefariousjosiah/WWE-SVR2008-RECOMP.git
cd WWE-SVR2008-RECOMP
powershell -ExecutionPolicy Bypass -File tools\windows\setup.ps1 -Iso "D:\path\to\your disc image.iso"
```

`setup.ps1` installs the tools it needs (Git, CMake, Ninja, LLVM clang, Python, 7-Zip, Visual Studio
2022 Build Tools), fetches the third-party sources at the pinned commits and applies this project's
patches (`patches/`), builds the ReXGlue SDK, extracts your disc image to `assets/`, recompiles
`default.xex` and builds the game.

The native renderer also needs your game's shaders, which can't be in this repository. The first
time, `setup.ps1` builds a shader-dump version and tells you how to collect them: play the menus
and a match or two with shader dumping on, run `tools\native\build_shader_cache.ps1`, then run
`setup.ps1` again.

Afterwards: `tools\windows\build.ps1` rebuilds, `tools\windows\run.ps1` runs it, and
`tools\windows\package_release.ps1` makes the release zip (version from `CMakeLists.txt`) (the game, no game
files).

Repository layout: `src/` the game-specific code (`native/` renderer glue, `reblue/` the renderer),
`config/` recompiler configuration, `patches/` changes to the third-party
sources, `tools/` build scripts, `docs/` the images on this page.

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom Clay, derived from
  [Xenia](https://xenia.jp): the recompiler and runtime.
- [re:Blue](https://github.com/zolaware/reblue) (Blue Dragon), the renderer this one is adapted
  from, with [plume](https://github.com/zolaware/plume) and
  [XenosRecomp](https://github.com/zolaware/reblue-XenosRecomp).
- The settings menu: [Dear ImGui](https://github.com/ocornut/imgui) and the Roboto font;
  [SDL](https://libsdl.org) for controllers and sound.
- [zstd](https://github.com/facebook/zstd); the Press Start 2P font.
- Inside the runtime: [FFmpeg](https://ffmpeg.org) (audio),
  [libmspack](https://github.com/kyz/libmspack) and the other libraries the ReXGlue SDK uses.

Licences of all of these: [THIRD_PARTY.md](THIRD_PARTY.md).

## Licence and legal

This project's own code, including the native renderer for SmackDown vs. Raw, was written by
nefariousjosiah and is licensed under the GNU General Public License v3.0 ([LICENSE](LICENSE)). The
releases are covered by the same licence, and this repository is their source. It builds on
re:Blue's renderer core (BSD 3-Clause) and the other projects in [THIRD_PARTY.md](THIRD_PARTY.md),
which keep their own licences; the releases include FFmpeg and libmspack under the LGPL (their
source: see THIRD_PARTY.md). None of the files from the game disc are in this repository or the
releases: the game's data (models, textures, sound, video) comes from your own disc image. The
releases do contain the game's program in recompiled form (`svr2008.exe`, with the game's shaders
converted for PC).

WWE, SmackDown vs. Raw and all related names, characters, content and the gameplay shown in the
showcase clips belong to their respective owners (WWE, THQ, Yuke's); the clips are used only to show
the port running. This is an unofficial fan project, not affiliated with or endorsed by them or by
Microsoft.
