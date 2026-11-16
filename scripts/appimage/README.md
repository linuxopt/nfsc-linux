# SpeedBreaker: Carbon AppImage

One file, `SpeedBreaker-Carbon-<version>-x86_64.AppImage`, that turns your own Xbox 360 disc image of
Need for Speed: Carbon (USA Collector's Edition) into a native Linux game on any x86-64 PC with an
AVX2 CPU and a Vulkan 1.2 GPU, built for that PC.

**The AppImage contains no game code and no game data**, so it can be copied to any number of
machines and shared: it holds SpeedBreaker's engine (GPL-3.0, its source is inside as
`usr/share/speedbreaker-carbon/source.tar.xz`), a compiler (clang/lld), XenonRecomp and the AI
upscaler. Every player needs their own disc image; the game is made from it on each PC.

## On each PC

1. Copy the AppImage, `chmod +x` it and run it.
2. **Play → Choose your disc image (ISO)…**. The launcher then, by itself:
   1. reads `default.xex` from the image and checks it is the USA Collector's Edition;
   2. translates its PowerPC code to C++ (XenonRecomp) and **compiles it for this PC's CPU**
      (`-march=native`), with the engine built for AVX-512 or AVX2 to match (about a minute on
      a 12-core CPU, a few minutes on a 4-core laptop);
   3. installs the game from the image, checking every file;
   4. tunes the graphics settings to the PC's hardware;
   5. builds the **AI textures** for the PC's GPU.
3. **PLAY**.

Scripted: `SpeedBreaker-Carbon-*.AppImage --setup /path/to/your.iso` does all of it without
clicks. The build lives in `~/.local/share/speedbreaker-carbon/engine/<cpu>/`, one per CPU
model, so a home folder shared between PCs (NFS, Syncthing) keeps a build for each CPU. A newer
AppImage (a newer engine) rebuilds it automatically the next time you press PLAY.

| Page | What it does |
|---|---|
| Play | Choose your disc image (build + install + AI textures), status, **Unlock all**, PLAY (builds whatever is missing first), the log |
| Graphics | Every video, audio and game setting from the game's own `settings.toml`; **Auto-detect the best settings for this PC** |
| AI Textures | Turn them on or off, pick a preset (Ultra / High / Fast / Off), build or rebuild, import a pack from another PC |
| Controls | Rebind every keyboard control (click, press a key; right-click clears), controller deadzones and vibration, steering wheel |
| Saves | Back up, restore, import, export, delete; a save editor (find a 32-bit value such as cash, narrow it down, set it) |
| Mods | Add (folder or archive), enable/disable, order, remove |
| DLC | Add your own Xbox 360 Marketplace packages (CON/LIVE files) or folders; remove |
| System | Hardware report, which GPU the game uses, **Rebuild the game for this PC**, data/log folders |

Command line: `--setup <iso>` (the whole first run), `--play` (straight into the game),
`--build-ai` (build the AI textures, then exit), `--game <args>` (this PC's game program itself:
`--where`, `--verify <iso>`, ...).

## Unlock all

**Play → Unlock all cars, parts, races and DLC** (off by default; also on the DLC page). The game's
own debug "unlock all things" flag (a byte at `0x82C6503C` that the 360 build still checks in 18
places: the car lot, career, parts, races) is set at boot, and the downloadable-content check says
every Marketplace item is installed, so the Collector's Edition and Marketplace cars on the disc are
all there. Saves never record it: turn it off and the game's own progress is back.
(`runtime/game/carbon/unlock_all.cpp`; `NFSMW_UNLOCK_ALL=1` when starting the game by hand.)

## Hardware auto-detection

- **CPU:** the game's code is compiled on the PC for its exact CPU (`-march=native`); the engine
  linked with it is the `x86-64-v4` (AVX-512) build on CPUs that have it, else `x86-64-v3`.
  Compile jobs are limited by the PC's free memory.
- **GPU tier** (from Vulkan: dedicated or integrated, video memory): Ultra (≥14 GB), High (≥9 GB),
  Medium (≥5 GB), Low (integrated, Steam Deck, or less).
- **Settings per tier and display:** internal resolution (1x–3x, matched to the screen's height),
  shadow resolution, anisotropic filtering, FSR 1 when the picture is enlarged a lot, sharpening,
  fullscreen, Smooth Motion on 90 Hz+ screens with a fast GPU. Re-tuned automatically when the
  launcher sees different hardware (a copied data folder); **System → Tune settings again** any time.
- **AI preset per tier:** Ultra/High → Real-ESRGAN x4plus, 4×, up to 2048 px; Medium → x4plus up to
  1024 px; Low → AnimeVideo v3 2×, up to 1024 px; Off without 2 GB of video memory.
- Laptops with NVIDIA beside another GPU get NVIDIA's PRIME offload; Steam Game Mode is detected.

## AI textures: how it works

1. `sb-texpack scan` reads the installed game's archives (`NFS/ZDIR.BIN` + `ZZDATA*.BIN`), finds
   every texture pack (plain and JDLZ/HUFF-compressed, including the streamed city), and writes each
   DXT texture ≥ 32 px as a PNG named by its **key**: a hash of its bytes exactly as the game loads
   them into memory. About 2,600 textures.
2. Real-ESRGAN (bundled, runs on the GPU through Vulkan) upscales them.
3. `sb-texpack encode` compresses them back to BC1/BC3 with mipmaps into
   `~/.local/share/speedbreaker-carbon/texture-pack/<key>.dds`.
4. The game (`runtime/gpu/texture_replace.*`) hashes each DXT texture it loads; when the pack has
   that key it samples the AI version instead, at its own resolution.

On an RX 9070 XT the Ultra build takes about 7 minutes and makes a 1.8 GB pack. To skip the
build on another of **your own** PCs, copy that `texture-pack` folder and use **AI Textures →
Import a pack**. **Never publish or share a texture pack:** it is made from the game's own art.
Textures the game assembles while it runs (car vinyls, for one) can't be matched beforehand and
stay as they are; normal maps are left out on purpose.

## Mods

`~/.local/share/speedbreaker-carbon/mods/<mod>/`:

- `mod.ini`: `name`, `description`, `author`, `version` (the launcher writes `enabled`, `order`)
- `textures/<key>.dds`: replacement textures, keyed as the AI pack (see `texture-pack/` for keys;
  the AI build's `~/.cache/speedbreaker-carbon/ai-work/manifest.tsv` names each key's texture)
- `files/...`: files that replace the game's own, by path (`files/NFS/ZZDATA3.BIN`, `files/Movies/...`)

Mods higher in the list win, and mod textures win over the AI pack.

## DLC

Carbon's Marketplace content was mostly car unlocks (Murciélago, Hemi Cuda, Corvette Z06, Shelby GT500,
AE86, custom car bundles). Add your own packages (from a console's
`Content/0000000000000000/454107EC/00000002/`); the launcher unpacks them into
`~/.local/share/speedbreaker-carbon/dlc/` and the game lists them as installed content.

## Known limits

- About 30% of the textures the front end loads are assembled by the game while it runs (car
  vinyls, for one): the pack can't match them beforehand, and they stay as they are.
- A replacement texture is read from disk when the game first loads it: a short hitch is possible
  the first time a new area or car appears.
- DLC import (CON/LIVE packages) and `files/` mods are implemented but untested with real content.
- The save editor writes raw bytes; the game may refuse an edited save (a backup is made first).
- Tested on CachyOS (Arch) with an AMD RX 9070 XT. Other distributions should work (the AppImage
  carries its own C library and runs on older systems through it) but are untested.

## Building the AppImage

```sh
scripts/setup_tools.sh
tools/extract-xiso/build/extract-xiso -d game-carbon/files -x /path/to/your.iso
tools/XenonRecomp/build/XenonRecomp/XenonRecomp config/carbon/carbon.toml tools/XenonRecomp/XenonUtils/ppc_context.h
cmake -S . -B build/v3 -G Ninja -DCMAKE_BUILD_TYPE=Release -DSB_GAME=carbon -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DSB_EXTRAS=ON
cmake -S . -B build/v4 -G Ninja -DCMAKE_BUILD_TYPE=Release -DSB_GAME=carbon -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DSB_X86_LEVEL=x86-64-v4
ninja -C build/v3 && ninja -C build/v4 SpeedBreaker
scripts/appimage/make_appimage.sh --realesrgan <unpacked realesrgan-ncnn-vulkan dir> --appimagetool <appimagetool>
```

Inputs that are not in this repository:

- **Real-ESRGAN ncnn-vulkan:**
  `https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-ubuntu.zip`
  (46,931,474 bytes, SHA-256 `e5aa6eb131234b87c0c51f82b89390f5e3e642b7b70f2b9bbe95b6a285a40c96`): unzip it
  and pass the folder to `--realesrgan`.
- **appimagetool** (https://github.com/AppImage/appimagetool): `make_appimage.sh` reuses the AppImage
  runtime inside it, so it downloads nothing.
- An Arch-based build machine (`make_sdk.sh` and `collect_licenses.sh` read pacman's package lists).

`make_appimage.sh` runs `make_sdk.sh`, which packs the engine as object files plus this machine's
clang, lld, XenonRecomp, glibc/libstdc++ headers and the libraries the game links; the extracted
game and the generated C++ (`game-carbon/`, `ppc-carbon/`) are only used to build the engine and
never go into the AppImage.

```
```
