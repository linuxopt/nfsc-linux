// SpeedBreaker launcher. GPL-3.0-or-later (see COPYING).
//
// The game's own icons, from the player's disc. default.xex carries the
// title's icon and its 50 achievements' (64x64 PNGs, in the XDBF resource the
// Xbox dashboard shows). None ship with the AppImage (they are the game's
// art): on each PC the launcher takes them from the installed default.xex,
// upscales them 4x with Real-ESRGAN, and uses them for its page icons, the
// launcher's and the game's title bar icon, and the applications menu entry.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace launcher::icons
{
    // The XDBF image ids: the title's icon, and the achievements' that stand
    // for the launcher's pages.
    inline constexpr uint32_t kTitle = 0x8000;
    inline constexpr uint32_t kPlay = 0x1f;      // nitro
    inline constexpr uint32_t kGraphics = 0x1b;  // a polished panel
    inline constexpr uint32_t kAi = 0x0d;        // a burst
    inline constexpr uint32_t kControls = 0x0c;  // a car and a signal
    inline constexpr uint32_t kSaves = 0x11;     // the garage
    inline constexpr uint32_t kMods = 0x23;      // a car being tuned
    inline constexpr uint32_t kDlc = 0x16;       // a line of cars
    inline constexpr uint32_t kSystem = 0x17;    // a grid
    inline constexpr uint32_t kUpdate = 0x1e;    // a car and a down arrow
    inline constexpr uint32_t kMovies = 0x12;    // Nikki

    // The id's file in a folder of them: "<id in hex>.png".
    std::filesystem::path File(const std::filesystem::path& dir, uint32_t id);

    // Every image in the xex's XDBF resource into dir. False (with why) for
    // an xex without one.
    bool Extract(const std::filesystem::path& xex, const std::filesystem::path& dir, std::string& error);

    // A square PNG as size x size RGBA (halved down, or bilinear up).
    bool LoadSquare(const std::filesystem::path& png, uint32_t size, std::vector<uint8_t>& rgba, std::string& error);

    // The application icon in the sizes desktops look for, from the title
    // icon (any size; square): dir/nfsc-linux-<n>.png, n = 256, 128, 64, 32, 16.
    bool MakeAppIcons(const std::filesystem::path& title, const std::filesystem::path& dir, std::string& error);
    std::filesystem::path AppIcon(const std::filesystem::path& dir, int size);

    // The applications menu entry (freedesktop.org: ~/.local/share/
    // applications/nfsc-linux.desktop and the hicolor icons), starting the
    // AppImage at `appImage`.
    inline constexpr const char kAppId[] = "nfsc-linux";
    bool InstallMenuEntry(const std::string& appImage, const std::filesystem::path& iconDir, std::string& error);
    bool RemoveMenuEntry(std::string& error);
    bool MenuEntryInstalled();
    // The AppImage the entry starts ("" when there is none).
    std::string MenuEntryTarget();
}
