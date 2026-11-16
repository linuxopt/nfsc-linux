// SpeedBreaker launcher. GPL-3.0-or-later (see COPYING).
//
// nfsc-linux updates: each time the launcher starts (unless turned off), one
// GET of GitHub's "latest release" of NFSC_GITHUB_REPO with the system's curl
// (as the game's own Check for Updates does, runtime/update/fetch_curl.cpp);
// when its version is newer than this one, the launcher offers to download
// its AppImage, check it against the release's .sha256, and put it in place
// of the running one (the old one kept as <AppImage>.previous).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#ifndef NFSC_VERSION
#define NFSC_VERSION "0.0.0"
#endif
#ifndef NFSC_GITHUB_REPO
#define NFSC_GITHUB_REPO "linuxopt/nfsc-linux"
#endif

namespace launcher::update
{
    inline constexpr const char kApiUrl[] = "https://api.github.com/repos/" NFSC_GITHUB_REPO "/releases/latest";
    inline constexpr const char kReleasesPage[] = "https://github.com/" NFSC_GITHUB_REPO "/releases/latest";
    // The release asset the updater takes (a stable name: shortcuts keep working).
    inline constexpr const char kAssetSuffix[] = "-x86_64.AppImage";

    struct Asset
    {
        std::string name, url;
        uint64_t size = 0;
    };

    struct Release
    {
        std::string tag, name, url, body;
        bool draft = false, prerelease = false;
        std::vector<Asset> assets;
        const Asset* Find(const std::string& suffix) const;
    };

    // GitHub's release JSON. False, with why, when it isn't one.
    bool ParseRelease(const std::string& json, Release& out, std::string& error);

    // "1.2.3", "v1.2.3", "1.2.3-rc.1": -1, 0 or 1 (a pre-release before its
    // release); false from Valid() for anything else.
    bool Valid(const std::string& version);
    int Compare(const std::string& a, const std::string& b);

    // The system's curl ("" when there is none).
    std::string FindCurl();
}
