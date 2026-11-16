# nfsc-linux

**A Linux-first fork of [SpeedBreaker: Carbon](https://github.com/SpeedBreakerProject/speedbreaker-carbon):
one AppImage that turns your own Xbox 360 disc image of *Need for Speed: Carbon* into a native Linux
game, compiled on your PC for your CPU, with AI-upscaled textures, a graphical launcher and
hardware auto-tuning.**

> [!IMPORTANT]
> This is an unofficial fan project. It is not affiliated with, endorsed by or sponsored by Electronic
> Arts Inc. or the Need for Speed brand, nor with project(u), the authors of SpeedBreaker. **No game
> code, game files or game assets are in this repository or in its releases.** You need your own legally
> obtained disc image of the **Xbox 360 USA Collector's Edition**; the game is made from it on your PC.
> Don't ask for or share game files, disc images, generated code or texture packs.

- [What's different from SpeedBreaker: Carbon](#whats-different-from-speedbreaker-carbon)
- [Download and play](#download-and-play)
- [The launcher](#the-launcher)
- [How it works](#how-it-works)
- [All changes, file by file](#all-changes-file-by-file)
- [Building from source](#building-from-source)
- [Known limits](#known-limits)
- [Credits and licenses](#credits-and-licenses)

## What's different from SpeedBreaker: Carbon

SpeedBreaker: Carbon (v0.1.3, by project(u)) is a static recompilation of the Xbox 360 game: its
PowerPC code translated to C++ with XenonRecomp and compiled into a native program, with a runtime
that stands in for the console. This fork keeps all of that and adds:

| | |
|---|---|
| **One shareable AppImage** | No game code inside: it carries the engine as object files, a compiler (clang/lld) and XenonRecomp. Each PC builds the game from its own disc image. |
| **Built for each PC's CPU** | The game's code is compiled on the PC with `-march=native`; the engine is linked in its AVX-512 (x86-64-v4) or AVX2 (x86-64-v3) build to match. |
| **AI-upscaled textures** | ~2,600 of the game's textures upscaled 2-4x with Real-ESRGAN on the PC's GPU and loaded by a new texture-replacement system in the renderer. |
| **Hardware auto-tuning** | GPU class and video memory, display, CPU and Steam Deck detection pick the render resolution, filtering, upscaling and AI quality. |
| **A graphical launcher** | Play, graphics, AI textures, controls, saves (with a value editor), mods, DLC and system pages. |
| **Mods** | Texture mods and file-replacement mods, enabled and ordered in the launcher. |
| **DLC** | Your own Xbox 360 Marketplace packages (CON/LIVE) unpacked and offered to the game. |
| **Its own versions and updates** | nfsc-linux is versioned on its own (from 1.0.0). The launcher checks this repository's newest release every time it starts and updates the AppImage in place; the game's *Check for Updates* asks this repository too. |
| **Unlock all** (opt-in) | The game's own debug "unlock all things" flag, plus the DLC check, for every car, part and race. Off by default; saves never record it. |

## Download and play

1. Download `NFSC-Linux-x86_64.AppImage` from the
   [latest release](https://github.com/linuxopt/nfsc-linux/releases/latest) (check it against its `.sha256`).
2. `chmod +x NFSC-Linux-x86_64.AppImage` and run it.
3. **Play → Choose your disc image (ISO)…** The launcher then:
   1. reads `default.xex` from the image and checks it's the USA Collector's Edition
      (SHA-256 `922c7a1a…bec1815`);
   2. translates its code with XenonRecomp and compiles it for this PC (about a minute on a
      12-core CPU, a few minutes on a laptop);
   3. installs the game from the image, checking all 78 files;
   4. tunes the settings for this PC's hardware;
   5. builds the AI textures for this PC's GPU (about 7 minutes on a fast GPU).
4. **PLAY.**

Or without clicks: `./NFSC-Linux-x86_64.AppImage --setup /path/to/your.iso`.

**Needs:** x86-64 Linux, a CPU with AVX2 (Intel Haswell / AMD Zen or later), a Vulkan 1.2 GPU
with BC texture support, about 8 GB of free space (5.4 GB game, ~2 GB AI textures) and ~1.5 GB of
memory per compile job (the build runs as many as memory allows).

**Several PCs:** copy the AppImage to each. Every PC builds its own game and texture pack. Builds
are kept per CPU model (`~/.local/share/speedbreaker-carbon/engine/<cpu>/`), so a shared home
folder works too, and a newer AppImage rebuilds the game automatically.

Command line: `--setup <iso>`, `--play`, `--build-ai`, `--game <args>` (the built game program
itself, e.g. `--where`).

## Versions and updates

nfsc-linux has its own version numbers, starting at **1.0.0** (it is based on SpeedBreaker: Carbon
0.1.3, which the launcher's System page names). Releases are tagged `v<version>` here, and each one
carries `NFSC-Linux-x86_64.AppImage` and its `.sha256`.

- **Every time the launcher starts** it asks GitHub's API for this repository's newest release (one
  request, with the system's `curl`; nothing else is sent). When it's newer, a banner offers
  **Update now** and **What's new** (the release notes).
- **Update now** downloads the new AppImage beside the running one, checks its size and SHA-256
  against the release's `.sha256`, keeps the old one as `NFSC-Linux-x86_64.AppImage.previous` and
  puts the new one in its place (same name: shortcuts keep working). **Restart now** starts it.
  When the folder isn't writable, or the launcher isn't running from the AppImage, it opens the
  download page instead.
- After an update the game is rebuilt on the next PLAY (a minute or so), since the engine changed;
  the installed game files, settings, saves and texture pack stay.
- **System → Updates**: the version, **Check now**, and the switch for the check at startup.
- The game's own *Check for Updates* (Settings > Advanced, in game) asks this repository too.

To publish an update: raise `SB_VERSION_CARBON` in `CMakeLists.txt`, add its `## v<version>` entry
to `CHANGELOG.md` (the game shows it once after updating), build the AppImage and attach
`NFSC-Linux-x86_64.AppImage` and its `.sha256` to a release tagged `v<version>`.

## The launcher

| Page | |
|---|---|
| **Play** | Choose the disc image; status of the build, install, AI textures, mods, DLC and saves; *Unlock all*; PLAY (builds whatever is missing first); the log |
| **Graphics** | Every video, audio and game option, read from the game's own `settings.toml` (labels, choices and ranges come from its comments); **Auto-detect the best settings for this PC** |
| **AI Textures** | On/off; presets Ultra (Real-ESRGAN x4plus, 4x, ≤2048 px), High (≤1024 px), Fast (AnimeVideo v3, 2x) and Off, with the one recommended for the PC marked; build, rebuild, import from another of your PCs |
| **Controls** | Rebind every keyboard control (click, press a key; right-click clears); controller deadzones and vibration; steering wheel |
| **Saves** | Back up, restore, import, export, delete (always backed up first); a save editor that finds a 32-bit value (cash, say), narrows it down and sets it |
| **Mods** | Add from a folder or archive, enable, reorder, remove |
| **DLC** | Add your Marketplace packages or folders; remove |
| **System** | Hardware report, which GPU the game uses, rebuild the game, folders |

### Auto-tuning

| Tier | GPU | Internal resolution | AI preset |
|---|---|---|---|
| Ultra | dedicated, ≥14 GB | up to 3x (2160p), supersampling below 4K | Ultra |
| High | dedicated, ≥9 GB | up to 2x | Ultra |
| Medium | dedicated, ≥5 GB | up to 1.5x | High |
| Low | integrated, Steam Deck, smaller | 1x (Deck: its own 1280x800) | Fast, or off under 2 GB |

The internal resolution follows the display's height; 16x anisotropic filtering from Medium up;
FSR 1 and more sharpening when the picture is enlarged a lot; Smooth Motion on 90 Hz+ displays from
High up; fullscreen. Settings are tuned on the first run and again when the launcher sees different
hardware.

## How it works

```
your.iso ──► default.xex ──► XenonRecomp ──► C++ ──► clang -march=native ──┐
                                                                          ├─► lld ──► the game (this PC)
          SpeedBreaker engine (objects, AVX-512 or AVX2 build, in the AppImage) ┘
your.iso ──► the game's installer ──► ~/.local/share/speedbreaker-carbon/game/
game files ──► sb-texpack scan ──► PNGs ──► Real-ESRGAN (GPU) ──► sb-texpack encode ──► texture-pack/*.dds
```

**AI textures.** The game's textures sit in EA texture packs (TPK) inside `NFS/ZDIR.BIN` +
`ZZDATA*.BIN`: plain, JDLZ- or HUFF-compressed in 32 KB blocks, and in the streamed city's
sections. A texture's bytes in those files are exactly the bytes the game later hands the Xbox 360
GPU, so `sb-texpack` can name each one by a hash of those bytes (XXH3 of the Xenos-tiled base level,
mixed with format and size) without running the game. The renderer computes the same key when it
loads a DXT1/DXT3/DXT5 texture and, when the pack has it, samples the upscaled BC1/BC3 version with
its own mip chain at its own resolution. Only block-compressed textures are replaced: no render
target is ever one, so a replacement can't stand in for something the GPU draws.

**Unlock all.** The Xbox 360 build still has EA's debug byte at `0x82C6503C` that the unlock system
(`sub_821E6458`), the car lot, career and front-end lists check (18 places); downloadable content goes
through the content manager's lookup (`sub_826407E8`) instead. With the option on, the flag is set at
boot and the lookup says every content id is installed; the profile serializer (`sub_824775B0`) runs
with the flag cleared, so saves never record it.

**DLC.** Content packages of type 2 are registered from `~/.local/share/speedbreaker-carbon/dlc/`
(one folder and a `.xcontent` each, as saves are); the launcher unpacks STFS packages there.

## All changes, file by file

Everything else is SpeedBreaker: Carbon v0.1.3 as released (commit `276cf59`). The project's own
history is in [CHANGELOG.md](CHANGELOG.md).

**Engine (`runtime/`)**

| File | Change |
|---|---|
| `runtime/gpu/texture_replace.{h,cpp}` | New: texture packs (`NFSMW_TEXTURE_PACK`, `NFSMW_TEXTURE_REPLACE=0`, `NFSMW_TEXTURE_KEYS=<file>` to log keys), DDS loading (BC1/2/3/5/7, RGBA8) |
| `runtime/gpu/renderer.cpp` | Hooks texture replacement into the texture load: a replacement image, format, levels and views per texture, sampled with a sampler for its mips; retired, never destroyed mid-frame |
| `runtime/game/carbon/unlock_all.cpp` | New: `NFSMW_UNLOCK_ALL=1` (see *How it works*) and `NFSMW_UNLOCK_LOG=1` |
| `runtime/kernel/xam.cpp` | DLC content: one folder per package under the data folder's `dlc/`, registered at startup |
| `runtime/report/build_info.cpp` | Names x86-64-v4 builds in the log |
| `runtime/game/game_info.h` | The app is *NFSC Linux*; its releases (Check for Updates, issues) are this repository's |
| `runtime/CMakeLists.txt`, `recomp/CMakeLists.txt`, `CMakeLists.txt` | `SB_X86_LEVEL` (x86-64-v3 or -v4), xxHash include, `SB_EXTRAS` for the tools below; nfsc-linux's version (`SB_VERSION_CARBON` 1.0.0, from SpeedBreaker: Carbon 0.1.3) and repository (`NFSC_GITHUB_REPO`) |
| `CHANGELOG.md` | The v1.0.0 entry (the game's "What's new" note) |

**New tools (`extras/`, built with `-DSB_EXTRAS=ON`)**

| File | |
|---|---|
| `extras/launcher/` | `sb-launcher`: the GUI (SDL3 + Dear ImGui on Vulkan): `main.cpp` (pages, jobs), `gfx.cpp` (window), `hardware.cpp` (detection, tiers, recommendations), `settings_file.cpp` (reads and edits `settings.toml` in place), `content.cpp` (saves, mods, DLC, STFS), `xiso.cpp` (reads `default.xex` from a disc image), `updater.cpp` (the release check, its JSON, versions), `process.cpp` (child processes) |
| `extras/texpack/` | `sb-texpack`: `scan` (archives, TPKs, JDLZ/HUFF/RAWW, Xenos untiling, DXT decoding, keys) and `encode` (PNG to BC1/BC3 DDS with mips) |

**AppImage (`scripts/appimage/`)**

| File | |
|---|---|
| `make_appimage.sh` | Builds `NFSC-Linux-x86_64.AppImage` (no game program inside) |
| `make_sdk.sh` | Packs the engine objects (v3 and v4), clang, lld, XenonRecomp, a sysroot (glibc, Linux and libstdc++ headers; C runtime) and link libraries |
| `build-game.sh` | Run by the launcher on each PC: XenonRecomp, parallel compile, link |
| `AppRun` | Chooses the bundled or system C/C++ runtime, NVIDIA PRIME offload, Steam Game Mode; starts the launcher or (`--game`) the PC's build |
| `collect_licenses.sh` | Lists every bundled package with its version, license and source, with the license texts |
| `settings-template.toml`, `licenses/`, `README.md` | First settings on a fresh PC; third-party license texts; the detailed AppImage guide |

**Docs:** this README; SpeedBreaker's own README is [docs/UPSTREAM-README.md](docs/UPSTREAM-README.md);
`THIRD_PARTY_NOTICES.md` gains the new components.

## Building from source

On an Arch-based system (the AppImage's SDK is made from its packages), with `clang lld cmake ninja
git python sdl3 vulkan-headers vulkan-icd-loader glslang spirv-tools pkgconf nasm zlib` installed and
your own disc image:

```sh
scripts/setup_tools.sh
tools/extract-xiso/build/extract-xiso -d game-carbon/files -x /path/to/your.iso
tools/XenonRecomp/build/XenonRecomp/XenonRecomp config/carbon/carbon.toml tools/XenonRecomp/XenonUtils/ppc_context.h
cmake -S . -B build/v3 -G Ninja -DCMAKE_BUILD_TYPE=Release -DSB_GAME=carbon \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DSB_EXTRAS=ON
cmake -S . -B build/v4 -G Ninja -DCMAKE_BUILD_TYPE=Release -DSB_GAME=carbon \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DSB_X86_LEVEL=x86-64-v4
ninja -C build/v3 && ninja -C build/v4 SpeedBreaker
scripts/appimage/make_appimage.sh --realesrgan <unzipped realesrgan-ncnn-vulkan> --appimagetool <appimagetool>
```

The Real-ESRGAN download (URL and SHA-256) and the rest are in
[scripts/appimage/README.md](scripts/appimage/README.md#building-the-appimage). The extracted game and
generated code (`game-carbon/`, `ppc-carbon/`) are only used to build the engine; they are git-ignored
and never go into the AppImage. `build/v3/runtime/SpeedBreaker` also runs directly, as upstream's
build does. The unit tests: add `-DSB_TESTS=ON`, then `ctest --test-dir <build dir>` (100 pass).

## Known limits

- About 30% of the textures the front end loads are assembled by the game at run time (car vinyls,
  for one) and can't be upscaled beforehand.
- A replacement is read from disk when the game first loads its texture: a short hitch is possible
  the first time a new area or car appears.
- DLC packages and `files/` mods are implemented but untested with real content; the save editor
  writes raw bytes and the game may refuse an edited save (a backup is always made first).
- Tested on CachyOS (Arch Linux) with a Ryzen 9 7900X3D and a Radeon RX 9070 XT: a locked 60 fps at
  3x internal resolution with the AI textures. Other distributions and GPUs are untested.
- Most Wanted (`-DSB_GAME=mw`) shares the engine changes but wasn't built here (it needs that
  game's files).
- Linux x86-64 only; upstream's macOS, Windows, iOS and SteamOS builds are untouched but untested
  with these changes.

## Credits and licenses

- **SpeedBreaker: Carbon** by project(u): the recompilation, runtime and everything this builds on.
  GPL-3.0-or-later ([COPYING](COPYING)); this fork is GPL-3.0-or-later too.
- **XenonRecomp** (hedge-dev, MIT), **Xenia** (BSD-3-Clause), and the rest of upstream's
  [third-party notices](THIRD_PARTY_NOTICES.md).
- **Real-ESRGAN** (Xintao Wang; models BSD-3-Clause, ncnn-vulkan program MIT) and **ncnn** (Tencent,
  BSD-3-Clause).
- **ea-compress** (the nfs-mw contributors, MIT OR Apache-2.0): the HUFF and JDLZ decoders `sb-texpack` ports.
- The AppImage's toolchain and libraries come from Arch Linux packages; each one's license, version and
  source is listed inside the AppImage (`usr/share/speedbreaker-carbon/licenses/README-packages.txt`).

*Need for Speed* and *Need for Speed: Carbon* are trademarks of Electronic Arts Inc. Xbox 360 is a
trademark of Microsoft.
