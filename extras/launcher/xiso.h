// SpeedBreaker launcher. GPL-3.0-or-later (see COPYING).
// Reads one file out of an Xbox 360 disc image (XDVDFS/XISO), so the
// launcher can take default.xex from the player's ISO and build the game
// from it before the game (and its own installer) exists on this PC.
#pragma once
#include <filesystem>
#include <string>

namespace launcher
{
    // `name` from the image's root folder (any case) into `dest`.
    bool ExtractFromIso(const std::filesystem::path& iso, const std::string& name, const std::filesystem::path& dest, std::string& error);
}
