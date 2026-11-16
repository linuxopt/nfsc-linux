// SpeedBreaker launcher. GPL-3.0-or-later (see COPYING).
//
// The player's content in the data folder:
//   save/<name>/ + save/<name>.xcontent   the game's saves (kernel/xam.cpp)
//   dlc/<name>/ + dlc/<name>.xcontent     downloadable content, unpacked from
//                                         the player's own Xbox 360 packages
//   mods/<name>/                          mods: mod.ini (name, description,
//                                         enabled, order), textures/ (texture
//                                         pack DDS files) and files/ (files
//                                         that replace the game's own)
//   save-backups/<date>/                  backups the launcher makes
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace launcher
{
    namespace fs = std::filesystem;

    struct SaveEntry
    {
        std::string name;         // the folder's
        std::string displayName;  // the game's (from the .xcontent)
        uint64_t bytes = 0;
        fs::file_time_type modified;
        std::vector<fs::path> files;
    };
    std::vector<SaveEntry> ListSaves(const fs::path& data);
    // A copy of save/ in save-backups/<now>/; returns its folder.
    bool BackupSaves(const fs::path& data, fs::path& made, std::string& error);
    std::vector<fs::path> ListSaveBackups(const fs::path& data);
    // Replaces save/ with the backup (the current saves are backed up first).
    bool RestoreSaves(const fs::path& data, const fs::path& backup, std::string& error);
    bool ExportSave(const fs::path& data, const SaveEntry& save, const fs::path& toFolder, std::string& error);
    // A folder holding <name>/ and <name>.xcontent (as ExportSave makes).
    bool ImportSave(const fs::path& data, const fs::path& fromFolder, std::string& error);
    bool DeleteSave(const fs::path& data, const SaveEntry& save, std::string& error);

    struct Mod
    {
        fs::path folder;
        std::string id, name, description, author, version;
        bool enabled = false;
        int order = 0;
        size_t textures = 0, files = 0;
    };
    std::vector<Mod> ListMods(const fs::path& data);
    bool SaveModState(const Mod& mod, std::string& error);
    // From a folder, or a .zip / .7z / .tar.* (unpacked with the system's bsdtar/unzip).
    bool AddMod(const fs::path& data, const fs::path& from, std::string& error);
    bool RemoveMod(const Mod& mod, std::string& error);
    // The enabled mods' texture folders, in order (for NFSMW_TEXTURE_PACK).
    std::vector<fs::path> ModTextureFolders(const std::vector<Mod>& mods);
    // When an enabled mod has files/: a folder of links that is the game
    // with those files in place (for NFSMW_GAME_DIR); empty when none has.
    bool BuildOverlay(const fs::path& game, const fs::path& cache, const std::vector<Mod>& mods, fs::path& overlay, std::string& error);

    struct DlcEntry
    {
        std::string name, displayName;
        uint64_t bytes = 0;
        size_t files = 0;
    };
    std::vector<DlcEntry> ListDlc(const fs::path& data);
    // An Xbox 360 content package (CON, LIVE or PIRS), or a folder of its
    // files: unpacked into dlc/<name>/ with its .xcontent.
    bool AddDlc(const fs::path& data, const fs::path& from, std::string& summary, std::string& error);
    bool RemoveDlc(const fs::path& data, const DlcEntry& dlc, std::string& error);

    uint64_t FolderBytes(const fs::path& p);
    std::string FormatBytes(uint64_t bytes);
}
