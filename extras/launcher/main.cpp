// SpeedBreaker launcher. GPL-3.0-or-later (see COPYING).
//
// The AppImage's front door: install the game from the player's own disc
// image, build the AI-upscaled texture pack for this computer, tune the
// settings to its hardware, edit graphics and controls, manage saves, mods
// and downloadable content, and start the game.
//
//   sb-launcher            the window
//   sb-launcher --play     start the game at once (with the pack and mods)
//   sb-launcher --build-ai build the AI texture pack without the window
//   sb-launcher --update   install a newer release if there is one, then exit
//   sb-launcher --setup <disc image>  the first run without the clicks: build
//                          the game for this PC, install it, build the AI textures
//
// Paths come from the AppImage's AppRun (SB_APPDIR, SB_LOADER, SB_LIBS,
// SB_GAME_BIN); without them, from beside this program (a development build).
#include "content.h"
#include "gfx.h"
#include "hardware.h"
#include "process.h"
#include "settings_file.h"
#include "xiso.h"
#include "updater.h"

#include <install/sha256.h>

#include <user/key_names.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace launcher;

namespace
{
    std::string Env(const char* name, const std::string& fallback = {})
    {
        const char* v = std::getenv(name);
        return v && *v ? v : fallback;
    }

    // ---- the launcher's own state (data/launcher.ini) ----
    struct State
    {
        std::map<std::string, std::string> values;
        fs::path path;
        void Load(const fs::path& p)
        {
            path = p;
            std::ifstream f(p);
            for (std::string line; std::getline(f, line);)
                if (size_t eq = line.find('='); eq != std::string::npos && line[0] != '#')
                    values[line.substr(0, eq)] = line.substr(eq + 1);
        }
        void Save()
        {
            std::error_code ec;
            fs::create_directories(path.parent_path(), ec);
            std::ofstream f(path, std::ios::trunc);
            f << "# SpeedBreaker: Carbon launcher (this computer's choices)\n";
            for (auto& [k, v] : values)
                f << k << "=" << v << "\n";
        }
        std::string Get(const std::string& k, const std::string& d = {}) const
        {
            auto it = values.find(k);
            return it == values.end() ? d : it->second;
        }
        void Set(const std::string& k, const std::string& v) { values[k] = v; Save(); }
    };

    // ---- a background job: steps run one after another ----
    struct Step
    {
        std::string title;
        std::function<std::vector<std::string>()> argv;   // empty: a step done in-process (run())
        std::function<bool(std::string&)> run;            // in-process work
        std::function<std::vector<std::string>()> env;
        std::function<float()> progress;                  // polled; <0: use parsed progress
        std::function<void(const std::string&)> line;     // each output line
        std::function<bool(int, std::string&)> done;      // exit code -> ok
    };

    struct Job
    {
        std::string name;
        std::deque<Step> steps;
        Process proc;
        bool active = false, failed = false, cancelled = false;
        std::string stepTitle, message;
        float parsed = -1;
        std::chrono::steady_clock::time_point started;
        std::function<void(bool)> finished;
    };

    struct PendingDialog
    {
        std::mutex mutex;
        std::string tag;
        std::vector<std::string> files;
        bool ready = false;
    };
    PendingDialog s_dialog;

    void SDLCALL DialogCallback(void* userdata, const char* const* filelist, int)
    {
        std::lock_guard lock(s_dialog.mutex);
        s_dialog.tag = static_cast<const char*>(userdata);
        s_dialog.files.clear();
        if (filelist)
            for (const char* const* f = filelist; *f; f++)
                s_dialog.files.push_back(*f);
        s_dialog.ready = true;
    }

    enum class Page { Play, Graphics, Ai, Controls, Saves, Mods, Dlc, System };

    void Help(const std::string& text);

    struct App
    {
        fs::path appDir, dataDir, cacheDir;
        fs::path sdkDir, buildScript;
        std::string sdkStamp;
        fs::path gameBin, texpackBin, esrganBin, esrganModels, settingsTemplate, fontPath;
        std::string loader, libs;
        Hardware hw;
        State state;
        SettingsFile settings;
        std::string settingsError;

        std::optional<fs::path> gameDir;
        bool whereChecked = false;
        std::vector<std::string> console;
        std::vector<std::string> foundImages;

        Job job;
        Process game;

        // Updates (updater.h): the startup check, and the release it found.
        enum class Upd { Idle, Checking, Current, Available, Failed, Installed };
        Upd upd = Upd::Idle;
        Process updateProc;
        std::vector<std::string> updateLines;
        update::Release latest;
        std::string updateMessage;
        bool updateDismissed = false, showNotes = false, restartRequested = false;
        std::string appImagePath = Env("APPIMAGE");  // set by the AppImage runtime
        bool gameRunning = false;
        Page page = Page::Play;
        std::string notice;

        // pages
        Option* capturing = nullptr;
        std::vector<SaveEntry> saves;
        std::vector<fs::path> backups;
        int selectedSave = -1, selectedFile = -1;
        std::vector<uint8_t> hexData;
        fs::path hexPath;
        int hexCursor = -1;
        bool hexDirty = false;
        char hexSearch[32] = "";
        std::vector<size_t> hexHits;
        std::vector<Mod> mods;
        std::vector<DlcEntry> dlc;
        bool confirmDelete = false;

        void Log(const std::string& s)
        {
            console.push_back(s);
            if (console.size() > 400)
                console.erase(console.begin(), console.begin() + 100);
        }

        // ---- paths ----
        void ResolvePaths()
        {
            char self[4096] = {};
            ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
            fs::path exeDir = n > 0 ? fs::path(std::string(self, size_t(n))).parent_path() : fs::current_path();
            appDir = Env("SB_APPDIR", (exeDir / "../..").lexically_normal().string());
            loader = Env("SB_LOADER");
            libs = Env("SB_LIBS");
            std::string xdgData = Env("XDG_DATA_HOME", Env("HOME") + "/.local/share");
            std::string xdgCache = Env("XDG_CACHE_HOME", Env("HOME") + "/.cache");
            dataDir = fs::path(xdgData) / "speedbreaker-carbon";
            cacheDir = fs::path(xdgCache) / "speedbreaker-carbon";
            auto first = [](std::initializer_list<fs::path> list) {
                for (const fs::path& p : list)
                    if (!p.empty() && fs::exists(p))
                        return p;
                return fs::path();
            };
            fs::path bin = appDir / "usr/bin", lib = appDir / "usr/lib", share = appDir / "usr/share/speedbreaker-carbon";
            texpackBin = first({ bin / "sb-texpack", exeDir / "sb-texpack" });
            esrganBin = first({ lib / "realesrgan/realesrgan-ncnn-vulkan", exeDir / "realesrgan/realesrgan-ncnn-vulkan" });
            esrganModels = esrganBin.empty() ? fs::path() : esrganBin.parent_path() / "models";
            settingsTemplate = first({ share / "settings-template.toml", exeDir / "settings-template.toml" });
            fontPath = first({ share / "Roboto-Medium.ttf", exeDir / "Roboto-Medium.ttf" });
            // The SDK the game is built with on this PC (scripts/appimage/make_sdk.sh).
            sdkDir = first({ lib / "sdk", exeDir / "sdk" });
            buildScript = sdkDir.empty() ? fs::path() : sdkDir / "build-game.sh";
            std::ifstream sdk(sdkDir / "SDK");
            for (std::string line; std::getline(sdk, line);)
                if (line.rfind("engine: ", 0) == 0)
                    sdkStamp = line.substr(8);
        }

        // The game is built on each PC, for its CPU (build-game.sh: its
        // own -march=native code), into a folder per CPU model, so a home
        // folder shared by several PCs keeps one build for each.
        std::string CpuKey() const
        {
            install::Sha256Digest d = install::Sha256Of(hw.cpu.data(), hw.cpu.size());
            std::string key;
            for (int i = 0; i < 6; i++)
                key += std::format("{:02x}", d[i]);
            return key;
        }
        fs::path EngineDir() const { return dataDir / "engine" / CpuKey(); }

        void PickGameBinary()
        {
            if (std::string forced = Env("SB_GAME_BIN"); !forced.empty())
                gameBin = forced;
            else
                gameBin = EngineDir() / "SpeedBreaker";
        }

        std::map<std::string, std::string> BuildInfo() const
        {
            std::map<std::string, std::string> info;
            std::ifstream f(EngineDir() / "build-info");
            for (std::string line; std::getline(f, line);)
                if (size_t eq = line.find('='); eq != std::string::npos)
                    info[line.substr(0, eq)] = line.substr(eq + 1);
            return info;
        }

        // Built for this PC with this AppImage's engine (a newer AppImage rebuilds).
        bool GameBuilt() const
        {
            if (!Env("SB_GAME_BIN").empty())
                return true;
            if (!fs::exists(gameBin))
                return false;
            auto info = BuildInfo();
            return sdkStamp.empty() || info["sdk"] == sdkStamp;
        }

        // A bundled program, through the AppImage's library loader (its
        // libraries: libgomp for Real-ESRGAN, zlib for sb-texpack).
        std::vector<std::string> Wrap(std::vector<std::string> argv) const
        {
            if (loader.empty())
                return argv;
            std::vector<std::string> w = { loader, "--library-path", libs, "--argv0", argv[0] };
            w.insert(w.end(), argv.begin(), argv.end());
            return w;
        }

        std::vector<std::string> GameArgv(const std::vector<std::string>& args)
        {
            std::vector<std::string> argv;
            if (!loader.empty())
            {
                argv = { loader, "--library-path", libs, "--argv0", gameBin.string(), gameBin.string() };
            }
            else
                argv = { gameBin.string() };
            argv.insert(argv.end(), args.begin(), args.end());
            return argv;
        }

        // ---- the game's install ----
        void CheckWhere()
        {
            Process p;
            if (!GameBuilt())
            {
                // No game program yet: the installer's default folder.
                gameDir.reset();
                if (fs::exists(dataDir / "game" / "default.xex"))
                    gameDir = dataDir / "game";
                whereChecked = true;
                return;
            }
            if (!p.Start(GameArgv({ "--where" })))
            {
                whereChecked = true;
                return;
            }
            std::vector<std::string> out;
            while (p.Running())
            {
                for (auto& l : p.Poll()) out.push_back(l);
                usleep(20000);
            }
            for (auto& l : p.Poll()) out.push_back(l);
            gameDir.reset();
            for (const std::string& l : out)
            {
                if (l.empty() || l[0] != '/')
                    continue;
                size_t paren = l.find(" (");
                fs::path p2 = l.substr(0, paren);
                if (fs::exists(p2 / "default.xex"))
                {
                    gameDir = p2;
                    break;
                }
            }
            whereChecked = true;
        }

        // ---- settings ----
        void LoadSettings()
        {
            fs::path path = dataDir / "settings.toml";
            std::error_code ec;
            if (!fs::exists(path) && !settingsTemplate.empty())
            {
                fs::create_directories(dataDir, ec);
                fs::copy_file(settingsTemplate, path, ec);
            }
            settingsError.clear();
            if (!settings.Load(path, settingsError))
                settingsError = "The game's settings file isn't there yet: start the game once.";
        }

        void SaveSettings()
        {
            std::string error;
            if (settings.Dirty() && !settings.Save(error))
                notice = "Settings: " + error;
        }

        void AutoTune(bool announce)
        {
            if (!settings.Loaded())
                return;
            auto rec = RecommendedSettings(hw);
            size_t changed = 0;
            for (auto& [sec, key, value] : rec)
            {
                Option* o = settings.Find(sec, key);
                if (o && SettingsFile::Text(*o) != value)
                {
                    settings.Set(sec, key, value);
                    changed++;
                }
            }
            SaveSettings();
            state.Set("fingerprint", hw.Fingerprint());
            if (announce)
                notice = std::format("Settings tuned for this computer ({} tier): {} changed.", TierName(hw.tier), changed);
        }

        // ---- AI textures ----
        fs::path PackDir() const { return dataDir / "texture-pack"; }
        size_t PackCount() const
        {
            size_t n = 0;
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(PackDir(), ec))
                n += e.path().extension() == ".dds";
            return n;
        }
        int AiPresetIndex() const
        {
            std::string v = state.Get("ai_preset");
            if (!v.empty())
                return std::clamp(std::atoi(v.c_str()), 0, int(AiPresets().size()) - 1);
            auto rec = RecommendedAi(hw);
            auto all = AiPresets();
            for (size_t i = 0; i < all.size(); i++)
                if (all[i].label == rec.label)
                    return int(i);
            return int(all.size()) - 1;
        }
        bool UnlockAll() const { return state.Get("unlock_all", "false") == "true"; }
        void UnlockAllCheckbox()
        {
            bool on = UnlockAll();
            if (ImGui::Checkbox("Unlock all cars, parts, races and DLC", &on))
                state.Set("unlock_all", on ? "true" : "false");
            Help("Every car (the Collector's Edition's and the Xbox Live Marketplace ones on the disc too), every part and "
                 "every race, from the start. Your saves don't record it: turn it off and the game's own progress is back.");
        }
        bool AiEnabled() const { return state.Get("ai_enabled", "true") == "true" && AiPresets()[AiPresetIndex()].enabled; }

        static size_t CountPng(const fs::path& dir)
        {
            size_t n = 0;
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(dir, ec))
                n += e.path().extension() == ".png";
            return n;
        }

        void StartAiBuild(std::function<void(bool)> then = {})
        {
            if (job.active)
                return;
            if (!gameDir)
            {
                notice = "Install the game first: AI textures are made from its files.";
                return;
            }
            if (texpackBin.empty() || esrganBin.empty())
            {
                notice = "The AI tools aren't in this build (sb-texpack, Real-ESRGAN).";
                return;
            }
            AiPreset preset = AiPresets()[AiPresetIndex()];
            if (!preset.enabled)
                return;
            fs::path work = cacheDir / "ai-work";
            auto total = std::make_shared<size_t>(0);
            job = {};
            job.name = "AI texture enhancement";
            job.finished = std::move(then);
            std::error_code ec;
            fs::remove_all(work, ec);
            fs::remove_all(dataDir / "texture-pack.new", ec);
            job.steps.push_back({ "Finding the game's textures",
                [=, this] { return Wrap({ texpackBin.string(), "scan", gameDir->string(), work.string() }); }, {}, {}, {},
                [=](const std::string& l) {
                    size_t a, b;
                    if (sscanf(l.c_str(), "done %zu", &a) == 1) *total = a;
                    (void)b;
                },
                [](int code, std::string& msg) { if (code) msg = "the texture scan failed"; return code == 0; } });
            int gpu = state.Get("gpu").empty() ? (hw.best >= 0 ? int(hw.gpus[hw.best].index) : 0) : std::atoi(state.Get("gpu").c_str());
            job.steps.push_back({ std::format("AI upscaling ({}x with {})", preset.scale, preset.model),
                [=, this] {
                    std::vector<std::string> a = { esrganBin.string(), "-i", (work / "src").string(), "-o", (work / "up").string(),
                        "-n", preset.model, "-s", std::to_string(preset.scale), "-f", "png", "-m", esrganModels.string(), "-g", std::to_string(gpu) };
                    // Less GPU memory: smaller tiles.
                    if (hw.best >= 0 && hw.gpus[hw.best].vram < (6ull << 30))
                        a.insert(a.end(), { "-t", "128" });
                    fs::create_directories(work / "up");
                    return Wrap(a);
                }, {}, {},
                [=] { return *total ? float(CountPng(work / "up")) / float(*total) : -1.0f; }, {},
                [=](int code, std::string& msg) {
                    size_t made = CountPng(work / "up");
                    if (made == 0) { msg = std::format("the AI upscaler made nothing (exit {}): does this GPU run Vulkan compute?", code); return false; }
                    return true;
                } });
            job.steps.push_back({ "Compressing the new textures",
                [=, this] {
                    return Wrap({ texpackBin.string(), "encode", work.string(), (work / "up").string(),
                        (dataDir / "texture-pack.new").string(), "--max", std::to_string(preset.maxSide) });
                }, {}, {}, {}, {},
                [](int code, std::string& msg) { if (code) msg = "compressing the textures failed"; return code == 0; } });
            job.steps.push_back({ "Installing the texture pack", {},
                [=, this](std::string& msg) {
                    std::error_code e;
                    fs::remove_all(PackDir(), e);
                    fs::rename(dataDir / "texture-pack.new", PackDir(), e);
                    if (e) { msg = e.message(); return false; }
                    fs::remove_all(work, e);
                    state.Set("ai_built", preset.label);
                    state.Set("ai_built_fingerprint", hw.Fingerprint());
                    return true;
                } });
            BeginJob();
        }

        // ---- install ----
        // Build the game for this PC from a default.xex (build-game.sh).
        Step BuildStep(fs::path xex)
        {
            return { "Building the game for this PC",
                [=, this] {
                    std::vector<std::string> a = { "/bin/bash", buildScript.string(), "--xex", xex.string(), "--out", EngineDir().string() };
                    if (std::string jobs = state.Get("build_jobs"); !jobs.empty())
                        a.insert(a.end(), { "--jobs", jobs });
                    return a;
                }, {}, {}, {},
                [this](const std::string& l) {
                    if (l == "stage translate") job.stepTitle = "Translating the game's PowerPC code to C++ (XenonRecomp)";
                    else if (l == "stage compile") job.stepTitle = std::format("Compiling it for this CPU ({} threads)", hw.threads);
                    else if (l == "stage link") job.stepTitle = "Linking the game";
                },
                [this](int code, std::string& msg) {
                    if (code) msg = code == 3 ? "that disc image isn't the USA Collector's Edition" : "the build failed (see the log)";
                    PickGameBinary();
                    return code == 0;
                } };
        }

        bool CanBuild()
        {
            if (buildScript.empty() || !fs::exists(buildScript))
            {
                notice = "This AppImage has no game SDK (usr/lib/sdk).";
                return false;
            }
            if (!hw.x86_64_v3)
            {
                notice = "This CPU lacks AVX2 (x86-64-v3): the game can't run here.";
                return false;
            }
            return true;
        }

        void AfterSetup(bool ok)
        {
            CheckWhere();
            LoadSettings();
            if (ok && state.Get("fingerprint") != hw.Fingerprint())
                AutoTune(true);
            // Then the AI enhancement, before the first race.
            if (ok && AiEnabled() && !PackCount())
                StartAiBuild();
        }

        // The player's disc image: its default.xex -> the game, built for
        // this PC -> the game's own installer (copies and checks every
        // file) -> the AI textures.
        void StartSetup(const std::string& image)
        {
            if (job.active || !CanBuild())
                return;
            fs::path xex = cacheDir / "setup" / "default.xex";
            job = {};
            job.name = "Setting up the game from your disc image";
            job.steps.push_back({ "Reading default.xex from the disc image", {},
                [=](std::string& msg) {
                    std::error_code ec;
                    fs::create_directories(xex.parent_path(), ec);
                    return ExtractFromIso(image, "default.xex", xex, msg);
                } });
            job.steps.push_back(BuildStep(xex));
            if (!gameDir)
                job.steps.push_back({ "Installing: copying and checking every file from your disc image",
                    [=, this] { return GameArgv({ "--install", image }); }, {}, {}, {},
                    [this](const std::string& l) {
                        int pct;
                        if (sscanf(l.c_str(), " %d%%", &pct) == 1) job.parsed = float(pct) / 100.0f;
                    },
                    [](int code, std::string& msg) { if (code) msg = "the install failed (see the log)"; return code == 0; } });
            job.steps.push_back({ "Cleaning up", {},
                [=](std::string&) { std::error_code ec; fs::remove_all(xex.parent_path(), ec); return true; } });
            job.finished = [this](bool ok) { AfterSetup(ok); };
            BeginJob();
        }

        // The game installed already (another AppImage, or an update): build it from its own default.xex.
        void StartBuild(std::function<void(bool)> then = {})
        {
            if (job.active || !CanBuild() || !gameDir)
                return;
            job = {};
            job.name = "Building the game for this PC";
            job.steps.push_back(BuildStep(*gameDir / "default.xex"));
            job.finished = [this, then](bool ok) {
                AfterSetup(ok);
                if (then && !job.active) then(ok);
            };
            BeginJob();
        }

        void FindImages()
        {
            Process p;
            foundImages.clear();
            if (!p.Start(GameArgv({ "--find-images" })))
                return;
            while (p.Running()) usleep(20000);
            for (auto& l : p.Poll())
            {
                Log(l);
                std::string t = l;
                if (t.rfind("  ready ", 0) == 0)
                {
                    t = t.substr(8);
                    while (!t.empty() && t[0] == ' ') t.erase(0, 1);
                    foundImages.push_back(t.substr(0, t.rfind("  (")));
                }
            }
            if (foundImages.empty())
                notice = "No disc images found in the usual folders: choose one.";
        }

        // ---- jobs ----
        void BeginJob()
        {
            job.active = true;
            job.started = std::chrono::steady_clock::now();
            NextStep();
        }

        void NextStep()
        {
            while (!job.steps.empty())
            {
                Step& s = job.steps.front();
                job.stepTitle = s.title;
                job.parsed = -1;
                Log("== " + s.title);
                if (!s.argv)
                {
                    std::string msg;
                    bool ok = s.run(msg);
                    job.steps.pop_front();
                    if (!ok)
                        return EndJob(false, msg);
                    continue;
                }
                std::vector<std::string> env = s.env ? s.env() : std::vector<std::string>{};
                if (!job.proc.Start(s.argv(), env))
                    return EndJob(false, "can't start " + s.argv()[0]);
                return;
            }
            EndJob(true, "");
        }

        void EndJob(bool ok, const std::string& msg)
        {
            job.active = false;
            job.failed = !ok;
            job.message = ok ? job.name + ": done." : job.name + " stopped: " + msg;
            notice = job.message;
            Log(job.message);
            if (job.finished)
            {
                auto f = std::move(job.finished);
                job.finished = nullptr;
                f(ok && !job.cancelled);
            }
        }

        void PollJob()
        {
            if (!job.active || job.steps.empty())
                return;
            Step& s = job.steps.front();
            for (std::string& l : job.proc.Poll())
            {
                if (s.line) s.line(l);
                if (l.rfind("progress ", 0) == 0)
                {
                    size_t a, b;
                    if (sscanf(l.c_str(), "progress %zu %zu", &a, &b) == 2 && b)
                        job.parsed = float(a) / float(b);
                    continue;
                }
                Log(l);
            }
            if (job.proc.Running())
                return;
            for (std::string& l : job.proc.Poll()) { if (s.line) s.line(l); Log(l); }
            std::string msg;
            int code = job.proc.ExitCode();
            bool ok = job.cancelled ? false : (s.done ? s.done(code, msg) : code == 0);
            if (job.cancelled) msg = "cancelled";
            job.steps.pop_front();
            if (!ok)
                return EndJob(false, msg.empty() ? std::format("exit code {}", code) : msg);
            NextStep();
        }

        float JobProgress()
        {
            if (!job.active || job.steps.empty())
                return 0;
            if (job.steps.front().progress)
            {
                float p = job.steps.front().progress();
                if (p >= 0) return std::min(p, 1.0f);
            }
            return std::max(0.0f, job.parsed);
        }

        // ---- play ----
        std::vector<std::string> GameEnv()
        {
            std::vector<std::string> env;
            mods = ListMods(dataDir);
            std::string packs;
            for (const fs::path& p : ModTextureFolders(mods))
                packs += (packs.empty() ? "" : ":") + p.string();
            if (AiEnabled() && PackCount())
                packs += (packs.empty() ? "" : ":") + PackDir().string();
            if (packs.empty())
                env.push_back("NFSMW_TEXTURE_REPLACE=0");
            else
                env.push_back("NFSMW_TEXTURE_PACK=" + packs);
            if (gameDir)
            {
                fs::path overlay;
                std::string error;
                if (!BuildOverlay(*gameDir, cacheDir, mods, overlay, error))
                    Log("mods: " + error);
                else if (!overlay.empty())
                    env.push_back("NFSMW_GAME_DIR=" + overlay.string());
            }
            if (std::string gpu = state.Get("gpu"); !gpu.empty())
                env.push_back("NFSMW_GPU=" + gpu);
            // Every car, part, race and DLC unlocked (runtime/game/carbon/unlock_all.cpp).
            env.push_back(std::string("NFSMW_UNLOCK_ALL=") + (UnlockAll() ? "1" : "0"));
            return env;
        }

        void Play()
        {
            if (gameRunning || job.active)
                return;
            SaveSettings();
            if (!GameBuilt())
            {
                if (gameDir)
                    StartBuild([this](bool ok) { if (ok) Play(); });
                else
                    notice = "Choose your disc image first: the game is built from it on this PC.";
                return;
            }
            if (AiEnabled() && !PackCount() && gameDir)
            {
                // First: the AI enhancement this computer's preset asks for.
                StartAiBuild([this](bool ok) { if (ok) Play(); });
                return;
            }
            auto env = GameEnv();
            for (auto& e : env) Log("env " + e);
            if (!game.Start(GameArgv({}), env))
            {
                notice = "Can't start the game: " + gameBin.string();
                return;
            }
            gameRunning = true;
            SDL_MinimizeWindow(gfx::Window());
        }

        void PollGame()
        {
            if (!gameRunning)
                return;
            for (auto& l : game.Poll())
                if (l.rfind("[textures]", 0) == 0 || l.rfind("[build]", 0) == 0 || l.rfind("[runtime]", 0) == 0 || l.rfind("[xam]", 0) == 0)
                    Log(l);
            if (!game.Running())
            {
                gameRunning = false;
                notice = game.ExitCode() == 0 ? "The game closed." : std::format("The game closed with code {} (its log is in {}/logs).", game.ExitCode(), dataDir.string());
                SDL_RestoreWindow(gfx::Window());
                SDL_RaiseWindow(gfx::Window());
                LoadSettings();  // the game's menu may have changed them
            }
        }

        // ---- dialogs ----
        void OpenFile(const char* tag, std::vector<SDL_DialogFileFilter> filters)
        {
            SDL_ShowOpenFileDialog(DialogCallback, const_cast<char*>(tag), gfx::Window(), filters.empty() ? nullptr : filters.data(),
                int(filters.size()), Env("HOME").c_str(), false);
        }
        void OpenFolder(const char* tag)
        {
            SDL_ShowOpenFolderDialog(DialogCallback, const_cast<char*>(tag), gfx::Window(), Env("HOME").c_str(), false);
        }

        void HandleDialog()
        {
            std::string tag;
            std::vector<std::string> files;
            {
                std::lock_guard lock(s_dialog.mutex);
                if (!s_dialog.ready)
                    return;
                s_dialog.ready = false;
                tag = s_dialog.tag;
                files = s_dialog.files;
            }
            if (files.empty())
                return;
            std::string error, summary;
            const std::string& f = files[0];
            if (tag == "iso")
                StartSetup(f);
            else if (tag == "mod-folder" || tag == "mod-archive")
            {
                notice = AddMod(dataDir, f, error) ? "Mod added: " + fs::path(f).filename().string() : "Mod: " + error;
                mods = ListMods(dataDir);
            }
            else if (tag == "dlc-file" || tag == "dlc-folder")
            {
                notice = AddDlc(dataDir, f, summary, error) ? "Downloadable content added: " + summary : "DLC: " + error;
                dlc = ListDlc(dataDir);
            }
            else if (tag == "save-import")
            {
                notice = ImportSave(dataDir, f, error) ? "Save imported (the saves before it are backed up)." : "Import: " + error;
                saves = ListSaves(dataDir);
            }
            else if (tag == "save-export" && selectedSave >= 0 && selectedSave < int(saves.size()))
                notice = ExportSave(dataDir, saves[selectedSave], f, error) ? "Save exported to " + f : "Export: " + error;
            else if (tag == "pack-import")
            {
                std::error_code ec;
                fs::remove_all(PackDir(), ec);
                fs::copy(f, PackDir(), fs::copy_options::recursive, ec);
                notice = ec ? "Import: " + ec.message() : std::format("Texture pack imported: {} textures.", PackCount());
                state.Set("ai_built", "imported from " + f);
            }
        }

        static void OpenInFileManager(const fs::path& p)
        {
            std::error_code ec;
            fs::create_directories(p, ec);
            Process proc;
            proc.Start({ "/usr/bin/xdg-open", p.string() });
        }

        // ---- updates ----
        bool CheckAtStart() const { return state.Get("update_check", "true") == "true"; }
        // This launcher's version (NFSC_TEST_VERSION=<x.y.z> pretends to be another: tests).
        static std::string RunningVersion()
        {
            static const std::string v = [] {
                std::string t = Env("NFSC_TEST_VERSION");
                return !t.empty() && update::Valid(t) ? t : std::string(NFSC_VERSION);
            }();
            return v;
        }

        void StartUpdateCheck()
        {
            if (upd == Upd::Checking)
                return;
            std::string curl = update::FindCurl();
            if (curl.empty())
            {
                upd = Upd::Failed;
                updateMessage = "Checking for updates needs curl, which this system doesn't have.";
                return;
            }
            updateLines.clear();
            // One GET of the newest release; the HTTP status on the last line.
            if (!updateProc.Start({ curl, "-q", "-sS", "-L", "--max-time", "15", "--connect-timeout", "8",
                    "-H", "Accept: application/vnd.github+json", "-H", "X-GitHub-Api-Version: 2022-11-28",
                    "-A", "nfsc-linux-launcher/" + RunningVersion(), "-w", "\n%{http_code}", update::kApiUrl }))
            {
                upd = Upd::Failed;
                updateMessage = "Couldn't start curl.";
                return;
            }
            upd = Upd::Checking;
        }

        void PollUpdate()
        {
            if (upd != Upd::Checking)
                return;
            for (auto& l : updateProc.Poll())
                updateLines.push_back(std::move(l));
            if (updateProc.Running())
                return;
            for (auto& l : updateProc.Poll())
                updateLines.push_back(std::move(l));
            int status = 0;
            if (!updateLines.empty())
            {
                status = std::atoi(updateLines.back().c_str());
                updateLines.pop_back();
            }
            std::string json;
            for (const std::string& l : updateLines)
                json += l + "\n";
            state.Set("update_last_check", std::to_string(std::time(nullptr)));
            if (updateProc.ExitCode() != 0 || status == 0)
            {
                upd = Upd::Failed;
                updateMessage = "Couldn't reach GitHub to check for updates (offline?).";
                Log("update: " + updateMessage);
                return;
            }
            if (status == 404)
            {
                upd = Upd::Current;
                updateMessage = "No release published yet.";
                return;
            }
            std::string error;
            if (status != 200 || !update::ParseRelease(json, latest, error))
            {
                upd = Upd::Failed;
                updateMessage = status != 200 ? std::format("GitHub answered {} to the update check.", status) : error;
                Log("update: " + updateMessage);
                return;
            }
            if (!update::Valid(latest.tag) || latest.draft || latest.prerelease || update::Compare(latest.tag, RunningVersion()) <= 0)
            {
                upd = Upd::Current;
                updateMessage = std::format("Up to date (v{}; newest release {}).", RunningVersion(), latest.tag);
                return;
            }
            upd = Upd::Available;
            updateMessage = std::format("NFSC Linux {} is out (this is v{}).", latest.tag, RunningVersion());
            Log("update: " + updateMessage);
        }

        // Download the new AppImage beside the running one, check it, swap it in.
        void StartUpdateInstall()
        {
            if (job.active || gameRunning || upd != Upd::Available)
                return;
            const update::Asset* asset = latest.Find(update::kAssetSuffix);
            if (!asset || appImagePath.empty())
            {
                OpenUrl(latest.url.empty() ? update::kReleasesPage : latest.url);
                return;
            }
            fs::path target = appImagePath;
            if (access(target.parent_path().c_str(), W_OK) != 0)
            {
                notice = "Can't update in place: " + target.parent_path().string() + " isn't writable. The download page is open.";
                OpenUrl(latest.url);
                return;
            }
            std::string curl = update::FindCurl();
            const update::Asset* sha = latest.Find(".AppImage.sha256");
            fs::path tmp = target.string() + ".download", tmpSha = target.string() + ".download.sha256";
            update::Asset a = *asset;
            update::Asset s = sha ? *sha : update::Asset{};
            std::string tag = latest.tag;
            job = {};
            job.name = "Updating to " + tag;
            job.steps.push_back({ std::format("Downloading {} ({})", a.name, FormatBytes(a.size)),
                [=] { return std::vector<std::string>{ curl, "-q", "-sS", "-L", "--fail", "--connect-timeout", "20", "-o", tmp.string(), a.url }; },
                {}, {},
                [=] { std::error_code ec; uint64_t n = fs::exists(tmp) ? fs::file_size(tmp, ec) : 0; return a.size ? float(n) / float(a.size) : -1.0f; },
                {}, [](int code, std::string& msg) { if (code) msg = "the download failed"; return code == 0; } });
            if (sha)
                job.steps.push_back({ "Downloading its checksum",
                    [=] { return std::vector<std::string>{ curl, "-q", "-sS", "-L", "--fail", "--connect-timeout", "20", "-o", tmpSha.string(), s.url }; },
                    {}, {}, {}, {}, [](int code, std::string& msg) { if (code) msg = "the checksum download failed"; return code == 0; } });
            job.steps.push_back({ "Checking and installing it", {},
                [=, this](std::string& msg) {
                    std::error_code ec;
                    if (a.size && fs::file_size(tmp, ec) != a.size)
                    {
                        msg = "the download is the wrong size";
                        fs::remove(tmp, ec);
                        return false;
                    }
                    if (sha)
                    {
                        std::ifstream sf(tmpSha);
                        std::string want;
                        sf >> want;
                        install::Sha256 h;
                        std::ifstream in(tmp, std::ios::binary);
                        std::vector<char> buf(1 << 20);
                        while (in.read(buf.data(), std::streamsize(buf.size())) || in.gcount() > 0)
                            h.Update(buf.data(), size_t(in.gcount()));
                        install::Sha256Digest d = h.Finish();
                        std::string got;
                        for (uint8_t b : d) got += std::format("{:02x}", b);
                        fs::remove(tmpSha, ec);
                        if (got != want)
                        {
                            msg = "the download doesn't match its checksum: nothing was changed";
                            fs::remove(tmp, ec);
                            return false;
                        }
                    }
                    chmod(tmp.c_str(), 0755);
                    fs::path previous = target.string() + ".previous";
                    fs::remove(previous, ec);
                    fs::rename(target, previous, ec);
                    if (ec) { msg = "can't move the current AppImage aside: " + ec.message(); return false; }
                    fs::rename(tmp, target, ec);
                    if (ec)
                    {
                        std::error_code ec2;
                        fs::rename(previous, target, ec2);
                        msg = "can't put the new AppImage in place: " + ec.message();
                        return false;
                    }
                    upd = Upd::Installed;
                    updateMessage = std::format("Updated to {}. Restart the launcher to use it (the old one is kept as {}).", tag, previous.filename().string());
                    return true;
                } });
            job.finished = [this](bool ok) { if (ok) notice = updateMessage; };
            BeginJob();
        }

        static void OpenUrl(const std::string& url)
        {
            Process p;
            p.Start({ "/usr/bin/xdg-open", url });
        }

        void UiUpdateBanner()
        {
            if (updateDismissed || (upd != Upd::Available && upd != Upd::Installed))
                return;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.22f, 0.12f, 1));
            ImGui::BeginChild("##update", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 1.6f), ImGuiChildFlags_Borders);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(updateMessage.c_str());
            ImGui::SameLine();
            if (upd == Upd::Available)
            {
                bool inPlace = !appImagePath.empty() && latest.Find(update::kAssetSuffix);
                if (ImGui::SmallButton(inPlace ? "Update now" : "Open download page"))
                    StartUpdateInstall();
                ImGui::SameLine();
                if (ImGui::SmallButton("What's new"))
                    showNotes = true;
            }
            else if (ImGui::SmallButton("Restart now"))
                restartRequested = true;
            ImGui::SameLine();
            if (ImGui::SmallButton("Later"))
                updateDismissed = true;
            ImGui::EndChild();
            ImGui::PopStyleColor();
            if (showNotes)
            {
                ImGui::OpenPopup("What's new");
                showNotes = false;
            }
            ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 40, ImGui::GetFontSize() * 26), ImGuiCond_Appearing);
            if (ImGui::BeginPopupModal("What's new"))
            {
                ImGui::Text("%s", latest.name.empty() ? latest.tag.c_str() : latest.name.c_str());
                ImGui::BeginChild("##notes", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.2f), ImGuiChildFlags_Borders);
                ImGui::TextWrapped("%s", latest.body.c_str());
                ImGui::EndChild();
                if (ImGui::Button("Close"))
                    ImGui::CloseCurrentPopup();
                ImGui::SameLine();
                if (ImGui::Button("Open the release page"))
                    OpenUrl(latest.url);
                ImGui::EndPopup();
            }
        }

        // ---- UI ----
        void Ui();
        void UiPlay();
        void UiOptions(const char* section, const char* filter = nullptr);
        void UiGraphics();
        void UiAi();
        void UiControls();
        void UiSaves();
        void UiMods();
        void UiDlc();
        void UiSystem();
        void UiJob();
        void UiUpdates();
    };

    // Big accent button.
    bool BigButton(const char* label, ImVec2 size, bool enabled = true)
    {
        if (!enabled) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.35f, 0.05f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.45f, 0.10f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75f, 0.28f, 0.02f, 1));
        bool r = ImGui::Button(label, size);
        ImGui::PopStyleColor(3);
        if (!enabled) ImGui::EndDisabled();
        return r;
    }

    void Help(const std::string& text)
    {
        if (text.empty())
            return;
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    void App::Ui()
    {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        float side = ImGui::GetFontSize() * 11;
        ImGui::BeginChild("##nav", ImVec2(side, 0), ImGuiChildFlags_Borders);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.15f, 1));
        ImGui::TextUnformatted("NFSC LINUX");
        ImGui::PopStyleColor();
        ImGui::TextDisabled("v" NFSC_VERSION);
        ImGui::Separator();
        struct Nav { Page page; const char* label; };
        const Nav nav[] = { { Page::Play, "Play" }, { Page::Graphics, "Graphics" }, { Page::Ai, "AI Textures" }, { Page::Controls, "Controls" },
            { Page::Saves, "Saves" }, { Page::Mods, "Mods" }, { Page::Dlc, "DLC" }, { Page::System, "System" } };
        for (const Nav& n : nav)
        {
            if (ImGui::Selectable(n.label, page == n.page, 0, ImVec2(0, ImGui::GetFontSize() * 1.8f)))
            {
                capturing = nullptr;
                page = n.page;
                if (page == Page::Saves) { saves = ListSaves(dataDir); backups = ListSaveBackups(dataDir); }
                if (page == Page::Mods) mods = ListMods(dataDir);
                if (page == Page::Dlc) dlc = ListDlc(dataDir);
                if (page == Page::Graphics || page == Page::Controls) LoadSettings();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##page", ImVec2(0, 0));
        UiUpdateBanner();
        if (!notice.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.2f, 1));
            ImGui::BeginChild("##notice", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 1.4f), ImGuiChildFlags_Borders);
            ImGui::TextWrapped("%s", notice.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::GetFontSize() * 2);
            if (ImGui::SmallButton("x"))
                notice.clear();
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
        UiJob();
        switch (page)
        {
        case Page::Play: UiPlay(); break;
        case Page::Graphics: UiGraphics(); break;
        case Page::Ai: UiAi(); break;
        case Page::Controls: UiControls(); break;
        case Page::Saves: UiSaves(); break;
        case Page::Mods: UiMods(); break;
        case Page::Dlc: UiDlc(); break;
        case Page::System: UiSystem(); break;
        }
        ImGui::EndChild();
        ImGui::End();
    }

    void App::UiJob()
    {
        if (!job.active)
            return;
        ImGui::BeginChild("##job", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 3.2f), ImGuiChildFlags_Borders);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - job.started).count();
        ImGui::Text("%s: %s (%.0f s)", job.name.c_str(), job.stepTitle.c_str(), secs);
        float p = JobProgress();
        ImGui::ProgressBar(p, ImVec2(-ImGui::GetFontSize() * 6, 0), p > 0 ? std::format("{:.0f}%", p * 100).c_str() : "working...");
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            job.cancelled = true;
            job.proc.Stop();
        }
        ImGui::EndChild();
    }

    void App::UiPlay()
    {
        ImGui::SeparatorText("Need for Speed: Carbon (Xbox 360), native");
        if (!whereChecked)
            CheckWhere();
        bool built = GameBuilt();
        if (gameDir)
            ImGui::Text("Game files: installed in %s", gameDir->string().c_str());
        else
            ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Game files: not installed on this computer.");
        if (built)
        {
            auto info = BuildInfo();
            ImGui::Text("Game program: built on this PC for its CPU (%s engine, %s code)", info["level"] == "v4" ? "AVX-512" : "AVX2",
                info["march"].c_str());
        }
        else
            ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Game program: not built on this PC yet.");
        if (!gameDir || !built)
        {
            ImGui::TextWrapped("Choose your own backup (disc image) of the Xbox 360 USA Collector's Edition. The launcher "
                "translates its code and compiles the game for this PC's CPU (a few minutes), installs and checks every "
                "file, then builds the AI textures for this PC's GPU.");
            if (BigButton("Choose your disc image (ISO)...", ImVec2(ImGui::GetFontSize() * 16, 0), !job.active))
                OpenFile("iso", { { "Xbox 360 disc image", "iso;xiso" }, { "All files", "*" } });
            if (gameDir && !built)
            {
                ImGui::SameLine();
                if (ImGui::Button("Build from the installed files") && !job.active)
                    StartBuild();
            }
        }
        size_t packCount = PackCount();
        AiPreset preset = AiPresets()[AiPresetIndex()];
        if (!AiEnabled())
            ImGui::Text("AI textures: off");
        else if (packCount)
            ImGui::Text("AI textures: %zu enhanced textures (%s)", packCount, state.Get("ai_built", preset.label).c_str());
        else
            ImGui::Text("AI textures: not built yet (%s); Play builds them first.", preset.label.c_str());
        size_t enabledMods = 0;
        for (const Mod& m : ListMods(dataDir)) enabledMods += m.enabled;
        ImGui::Text("Mods: %zu enabled   DLC: %zu   Saves: %zu", enabledMods, ListDlc(dataDir).size(), ListSaves(dataDir).size());
        ImGui::Text("This computer: %s tier (%s)", TierName(hw.tier), hw.best >= 0 ? hw.gpus[hw.best].name.c_str() : "no GPU");
        UnlockAllCheckbox();
        ImGui::Spacing();
        const char* label = gameRunning ? "Playing..." : !built ? "BUILD & PLAY" : (AiEnabled() && !packCount && gameDir) ? "ENHANCE & PLAY" : "PLAY";
        if (BigButton(label, ImVec2(ImGui::GetFontSize() * 14, ImGui::GetFontSize() * 3), gameDir && !gameRunning && !job.active && hw.x86_64_v3))
            Play();
        if (!hw.x86_64_v3)
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "This CPU lacks AVX2 (x86-64-v3): the game can't run here.");
        if (gameRunning)
        {
            ImGui::SameLine();
            if (ImGui::Button("Stop the game"))
                game.Stop();
        }
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Log"))
        {
            ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
            for (const std::string& l : console)
                ImGui::TextUnformatted(l.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
        }
    }

    void App::UiOptions(const char* section, const char* filter)
    {
        for (Option& o : settings.Options())
        {
            if (o.section != section || (filter && o.key.find(filter) == std::string::npos))
                continue;
            ImGui::PushID(&o);
            std::string label = o.label + (o.nextLaunch ? " *" : "");
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label.c_str());
            Help(o.description + (o.nextLaunch ? " (Applies at the next launch.)" : ""));
            ImGui::SameLine(ImGui::GetFontSize() * 16);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
            std::string text = SettingsFile::Text(o);
            switch (o.kind)
            {
            case Kind::Bool:
            {
                bool v = text == "true";
                if (ImGui::Checkbox("##v", &v)) { settings.SetBool(o, v); SaveSettings(); }
                break;
            }
            case Kind::Choice:
            {
                if (ImGui::BeginCombo("##v", text.c_str()))
                {
                    for (const std::string& c : o.choices)
                        if (ImGui::Selectable(c.c_str(), c == text)) { settings.SetText(o, c); SaveSettings(); }
                    ImGui::EndCombo();
                }
                break;
            }
            case Kind::Int:
            {
                int v = std::atoi(text.c_str());
                if (ImGui::SliderInt("##v", &v, int(o.min), int(o.max))) settings.SetNumber(o, v);
                if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
                break;
            }
            case Kind::Float:
            {
                float v = float(std::atof(text.c_str()));
                if (ImGui::SliderFloat("##v", &v, float(o.min), float(o.max), "%.2f")) settings.SetNumber(o, v);
                if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();
                break;
            }
            default:
                ImGui::TextDisabled("%s", text.c_str());
            }
            ImGui::PopID();
        }
    }

    void App::UiGraphics()
    {
        if (!settings.Loaded())
        {
            ImGui::TextWrapped("%s", settingsError.c_str());
            return;
        }
        if (BigButton("Auto-detect the best settings for this PC", ImVec2(0, 0)))
            AutoTune(true);
        Help("Picks the internal resolution, shadows, filtering and upscaling from this computer's GPU, its memory and the display.");
        ImGui::SameLine();
        ImGui::TextDisabled("%s tier", TierName(hw.tier));
        ImGui::TextDisabled("Settings marked * apply at the next launch. The game's own menu (F1) changes these too.");
        if (ImGui::BeginTabBar("##gfx"))
        {
            if (ImGui::BeginTabItem("Video")) { UiOptions("video"); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Audio")) { UiOptions("audio"); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Game")) { UiOptions("game"); UiOptions("advanced"); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }

    void App::UiAi()
    {
        ImGui::SeparatorText("AI texture enhancement");
        ImGui::TextWrapped("The game's textures are taken from your installed copy, enlarged with Real-ESRGAN (a neural network "
            "upscaler running on your GPU), compressed again and loaded by the game in place of the originals. "
            "Nothing leaves this computer.");
        bool enabled = state.Get("ai_enabled", "true") == "true";
        if (ImGui::Checkbox("Use AI textures in the game", &enabled))
            state.Set("ai_enabled", enabled ? "true" : "false");
        auto presets = AiPresets();
        AiPreset rec = RecommendedAi(hw);
        int current = AiPresetIndex();
        for (size_t i = 0; i < presets.size(); i++)
        {
            std::string label = presets[i].label + (presets[i].label == rec.label ? "   (recommended for this PC)" : "");
            if (ImGui::RadioButton(label.c_str(), current == int(i)))
                state.Set("ai_preset", std::to_string(i));
        }
        ImGui::Spacing();
        size_t count = PackCount();
        if (count)
            ImGui::Text("Installed: %zu textures, %s (%s)", count, FormatBytes(FolderBytes(PackDir())).c_str(), state.Get("ai_built", "?").c_str());
        else
            ImGui::Text("No AI texture pack on this computer yet.");
        if (count && state.Get("ai_built_fingerprint") != hw.Fingerprint() && !state.Get("ai_built_fingerprint").empty())
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "It was built on other hardware: it works, but a rebuild suits this PC.");
        ImGui::TextDisabled("About 2,600 textures. On a fast GPU the Ultra preset takes around 10 minutes; Fast is 3-4x quicker.");
        bool can = gameDir && !job.active && presets[current].enabled;
        if (BigButton(count ? "Rebuild AI textures" : "Build AI textures", ImVec2(0, 0), can))
            StartAiBuild();
        ImGui::SameLine();
        if (ImGui::Button("Import a pack from another PC...") && !job.active)
            OpenFolder("pack-import");
        Help("Copy the texture-pack folder from another computer's data folder to skip building it again.");
        ImGui::SameLine();
        if (ImGui::Button("Open pack folder"))
            OpenInFileManager(PackDir());
        if (count)
        {
            ImGui::SameLine();
            if (ImGui::Button("Delete pack"))
            {
                std::error_code ec;
                fs::remove_all(PackDir(), ec);
            }
        }
    }

    void App::UiControls()
    {
        if (!settings.Loaded())
        {
            ImGui::TextWrapped("%s", settingsError.c_str());
            return;
        }
        if (ImGui::BeginTabBar("##controls"))
        {
            if (ImGui::BeginTabItem("Keyboard"))
            {
                ImGui::TextDisabled("Click a key, then press the new one. Right-click clears it. Controllers work as an Xbox 360 pad.");
                if (ImGui::BeginTable("##keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY))
                {
                    ImGui::TableSetupColumn("Control");
                    ImGui::TableSetupColumn("Key");
                    ImGui::TableSetupColumn("Second key");
                    ImGui::TableHeadersRow();
                    auto& opts = settings.Options();
                    for (size_t i = 0; i < opts.size(); i++)
                    {
                        Option& o = opts[i];
                        if (o.section != "keys" || o.key.ends_with("_2"))
                            continue;
                        Option* second = settings.Find("keys", o.key + "_2");
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(o.label.c_str());
                        Help(o.description);
                        for (Option* k : { &o, second })
                        {
                            ImGui::TableNextColumn();
                            if (!k) continue;
                            ImGui::PushID(k);
                            std::string text = capturing == k ? "press a key..." : SettingsFile::Text(*k);
                            if (ImGui::Button(text.c_str(), ImVec2(-1, 0)))
                                capturing = k;
                            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                            {
                                settings.SetText(*k, "none");
                                SaveSettings();
                            }
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Controller")) { UiOptions("input"); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Steering wheel")) { UiOptions("wheel"); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }

    void App::UiSaves()
    {
        ImGui::SeparatorText("Saves");
        if (ImGui::Button("Back up all saves")) { fs::path made; std::string e; notice = BackupSaves(dataDir, made, e) ? "Backed up to " + made.string() : "Backup: " + e; backups = ListSaveBackups(dataDir); }
        ImGui::SameLine();
        if (ImGui::Button("Import a save...")) OpenFolder("save-import");
        ImGui::SameLine();
        if (ImGui::Button("Open saves folder")) OpenInFileManager(dataDir / "save");
        if (gameRunning)
            ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "The game is running: close it before changing saves.");
        if (saves.empty())
            ImGui::TextDisabled("No saves yet: they appear after the game first saves.");
        if (ImGui::BeginTable("##saves", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
        {
            ImGui::TableSetupColumn("Save");
            ImGui::TableSetupColumn("Size");
            ImGui::TableSetupColumn("Changed");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (int i = 0; i < int(saves.size()); i++)
            {
                const SaveEntry& s = saves[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(s.displayName.c_str(), selectedSave == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                {
                    selectedSave = i;
                    selectedFile = -1;
                    hexData.clear();
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(FormatBytes(s.bytes).c_str());
                ImGui::TableNextColumn();
                auto sys = std::chrono::clock_cast<std::chrono::system_clock>(s.modified);
                ImGui::TextUnformatted(std::format("{:%Y-%m-%d %H:%M}", std::chrono::floor<std::chrono::minutes>(sys)).c_str());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Export")) { selectedSave = i; OpenFolder("save-export"); }
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete")) { selectedSave = i; confirmDelete = true; }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (confirmDelete && selectedSave >= 0 && selectedSave < int(saves.size()))
        {
            ImGui::OpenPopup("Delete save?");
            confirmDelete = false;
        }
        if (ImGui::BeginPopupModal("Delete save?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Delete \"%s\"? All saves are backed up first.", saves[selectedSave].displayName.c_str());
            if (ImGui::Button("Delete"))
            {
                std::string e;
                notice = DeleteSave(dataDir, saves[selectedSave], e) ? "Save deleted (a backup is in save-backups)." : "Delete: " + e;
                saves = ListSaves(dataDir);
                selectedSave = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep it")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (!backups.empty() && ImGui::CollapsingHeader(std::format("Backups ({})", backups.size()).c_str()))
            for (const fs::path& b : backups)
            {
                ImGui::PushID(b.c_str());
                if (ImGui::SmallButton("Restore"))
                {
                    std::string e;
                    notice = RestoreSaves(dataDir, b, e) ? "Restored " + b.filename().string() + " (the saves before it are backed up)." : "Restore: " + e;
                    saves = ListSaves(dataDir);
                    backups = ListSaveBackups(dataDir);
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(b.filename().string().c_str());
                ImGui::PopID();
            }
        // The save editor: the files of the selected save, as bytes.
        if (selectedSave < 0 || selectedSave >= int(saves.size()))
            return;
        ImGui::SeparatorText("Save editor");
        ImGui::TextWrapped("Edit a save's bytes directly. To change a number (cash, for one), search for its current value, "
            "change it in the game a little, search again to narrow the matches down, then set it. The game checks its "
            "saves: one it doesn't accept is restored from the backup made before every write.");
        const SaveEntry& s = saves[selectedSave];
        for (int i = 0; i < int(s.files.size()); i++)
        {
            std::string rel = fs::relative(s.files[i], dataDir / "save" / s.name).string();
            if (ImGui::RadioButton(rel.c_str(), selectedFile == i))
            {
                selectedFile = i;
                hexPath = s.files[i];
                std::ifstream f(hexPath, std::ios::binary);
                hexData.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
                hexCursor = -1;
                hexDirty = false;
                hexHits.clear();
            }
        }
        if (hexData.empty())
            return;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::InputText("Value to find", hexSearch, sizeof(hexSearch), ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        auto search = [&](bool narrow) {
            uint32_t v = uint32_t(std::strtoul(hexSearch, nullptr, 10));
            std::vector<size_t> hits;
            auto match = [&](size_t i) {
                if (i + 4 > hexData.size()) return false;
                uint32_t be = uint32_t(hexData[i]) << 24 | uint32_t(hexData[i + 1]) << 16 | uint32_t(hexData[i + 2]) << 8 | hexData[i + 3];
                return be == v;
            };
            if (narrow) { for (size_t h : hexHits) if (match(h)) hits.push_back(h); }
            else for (size_t i = 0; i + 4 <= hexData.size(); i++) if (match(i)) hits.push_back(i);
            hexHits = hits;
        };
        if (ImGui::Button("Find (32-bit)")) search(false);
        ImGui::SameLine();
        if (ImGui::Button("Narrow") && !hexHits.empty())
        {
            std::ifstream f(hexPath, std::ios::binary);
            hexData.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            search(true);
        }
        ImGui::SameLine();
        ImGui::Text("%zu matches", hexHits.size());
        for (size_t k = 0; k < std::min<size_t>(hexHits.size(), 8); k++)
        {
            size_t h = hexHits[k];
            ImGui::PushID(int(h));
            static int newValue = 0;
            ImGui::Text("at %06zX", h);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
            ImGui::InputInt("##nv", &newValue, 0);
            ImGui::SameLine();
            if (ImGui::Button("Set"))
            {
                uint32_t v = uint32_t(newValue);
                hexData[h] = uint8_t(v >> 24); hexData[h + 1] = uint8_t(v >> 16); hexData[h + 2] = uint8_t(v >> 8); hexData[h + 3] = uint8_t(v);
                hexDirty = true;
            }
            ImGui::PopID();
        }
        if (hexDirty)
        {
            if (BigButton("Write the save (backs up first)", ImVec2(0, 0), !gameRunning))
            {
                fs::path made;
                std::string e;
                if (BackupSaves(dataDir, made, e))
                {
                    std::ofstream f(hexPath, std::ios::binary | std::ios::trunc);
                    f.write(reinterpret_cast<const char*>(hexData.data()), std::streamsize(hexData.size()));
                    notice = f ? "Save written; the backup is in " + made.string() : "Couldn't write the save.";
                    hexDirty = false;
                }
                else
                    notice = "Backup failed, nothing written: " + e;
            }
        }
        ImGui::BeginChild("##hex", ImVec2(0, ImGui::GetFontSize() * 14), ImGuiChildFlags_Borders);
        ImGuiListClipper clip;
        clip.Begin(int((hexData.size() + 15) / 16));
        while (clip.Step())
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++)
            {
                std::string line = std::format("{:06X}  ", row * 16);
                std::string ascii;
                for (int c = 0; c < 16; c++)
                {
                    size_t i = size_t(row) * 16 + c;
                    if (i < hexData.size())
                    {
                        line += std::format("{:02X} ", hexData[i]);
                        ascii.push_back(hexData[i] >= 32 && hexData[i] < 127 ? char(hexData[i]) : '.');
                    }
                    else
                        line += "   ";
                }
                ImGui::TextUnformatted((line + " " + ascii).c_str());
            }
        ImGui::EndChild();
    }

    void App::UiMods()
    {
        ImGui::SeparatorText("Mods");
        if (ImGui::Button("Add mod folder...")) OpenFolder("mod-folder");
        ImGui::SameLine();
        if (ImGui::Button("Add mod archive...")) OpenFile("mod-archive", { { "Archives", "zip;7z;rar;tar;gz;xz;zst" } });
        ImGui::SameLine();
        if (ImGui::Button("Open mods folder")) OpenInFileManager(dataDir / "mods");
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) mods = ListMods(dataDir);
        ImGui::TextDisabled("A mod is a folder with textures/ (DDS files named by texture key, as in the AI pack) and/or files/ "
            "(files that replace the game's, e.g. files/NFS/ZZDATA3.BIN). Higher in the list wins. mod.ini holds its name.");
        bool changed = false;
        for (int i = 0; i < int(mods.size()); i++)
        {
            Mod& m = mods[i];
            ImGui::PushID(i);
            if (ImGui::Checkbox("##on", &m.enabled)) changed = true;
            ImGui::SameLine();
            ImGui::Text("%s%s%s", m.name.c_str(), m.version.empty() ? "" : " ", m.version.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%zu textures, %zu files%s%s", m.textures, m.files, m.author.empty() ? "" : ", by ", m.author.c_str());
            Help(m.description);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::GetFontSize() * 9);
            if (ImGui::ArrowButton("up", ImGuiDir_Up) && i > 0) { std::swap(mods[i], mods[i - 1]); changed = true; }
            ImGui::SameLine();
            if (ImGui::ArrowButton("down", ImGuiDir_Down) && i + 1 < int(mods.size())) { std::swap(mods[i], mods[i + 1]); changed = true; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove"))
            {
                std::string e;
                notice = RemoveMod(m, e) ? "Removed " + m.name : "Remove: " + e;
                mods = ListMods(dataDir);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if (changed)
        {
            std::string e;
            for (int i = 0; i < int(mods.size()); i++)
            {
                mods[i].order = i;
                SaveModState(mods[i], e);
            }
        }
        if (mods.empty())
            ImGui::TextDisabled("No mods installed.");
    }

    void App::UiDlc()
    {
        ImGui::SeparatorText("Downloadable content");
        ImGui::TextWrapped("Add the Xbox 360 Marketplace content you own for NFS: Carbon (car unlocks such as the Murcielago, "
            "Hemi Cuda, Corvette Z06, Shelby GT500 or the AE86, and the custom car bundles): the package file "
            "(a CON or LIVE file, from your console's Content/0000000000000000/454107EC/00000002 folder) or a folder of its files.");
        UnlockAllCheckbox();
        if (ImGui::Button("Add package file...")) OpenFile("dlc-file", { { "Xbox 360 content package", "*" } });
        ImGui::SameLine();
        if (ImGui::Button("Add folder...")) OpenFolder("dlc-folder");
        ImGui::SameLine();
        if (ImGui::Button("Open DLC folder")) OpenInFileManager(dataDir / "dlc");
        for (int i = 0; i < int(dlc.size()); i++)
        {
            ImGui::PushID(i);
            ImGui::BulletText("%s  (%zu files, %s)", dlc[i].displayName.c_str(), dlc[i].files, FormatBytes(dlc[i].bytes).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove"))
            {
                std::string e;
                notice = RemoveDlc(dataDir, dlc[i], e) ? "Removed." : e;
                dlc = ListDlc(dataDir);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if (dlc.empty())
            ImGui::TextDisabled("None added.");
    }

    void App::UiUpdates()
    {
        ImGui::SeparatorText("Updates");
        ImGui::Text("NFSC Linux v%s (based on SpeedBreaker: Carbon 0.1.3), from github.com/%s", RunningVersion().c_str(), NFSC_GITHUB_REPO);
        bool atStart = CheckAtStart();
        if (ImGui::Checkbox("Check for updates each time the launcher starts", &atStart))
            state.Set("update_check", atStart ? "true" : "false");
        Help("One request to GitHub's releases API for this project's newest release, nothing else.");
        if (ImGui::Button("Check now") && upd != Upd::Checking)
        {
            updateDismissed = false;
            StartUpdateCheck();
        }
        ImGui::SameLine();
        if (upd == Upd::Checking)
            ImGui::TextDisabled("Asking GitHub...");
        else if (!updateMessage.empty())
            ImGui::TextUnformatted(updateMessage.c_str());
        if (upd == Upd::Available)
        {
            if (BigButton(appImagePath.empty() ? "Open the download page" : "Update now", ImVec2(0, 0), !job.active && !gameRunning))
                StartUpdateInstall();
            ImGui::SameLine();
            if (ImGui::Button("What's new"))
            {
                updateDismissed = false;
                showNotes = true;
            }
        }
        if (appImagePath.empty())
            ImGui::TextDisabled("Not running as an AppImage: updates open the download page instead.");
        else if (fs::exists(appImagePath + ".previous"))
        {
            ImGui::TextDisabled("The version before the last update: %s.previous", fs::path(appImagePath).filename().string().c_str());
        }
    }

    void App::UiSystem()
    {
        UiUpdates();
        ImGui::SeparatorText("This computer");
        ImGui::TextUnformatted(hw.Describe().c_str());
        ImGui::Text("Tier: %s   Free space: %s", TierName(hw.tier), FormatBytes(hw.freeDisk).c_str());
        ImGui::Text("Engine: %s", sdkStamp.c_str());
        ImGui::SeparatorText("GPU for the game");
        std::string current = state.Get("gpu");
        if (ImGui::RadioButton("Automatic (a dedicated GPU first)", current.empty()))
            state.Set("gpu", "");
        for (const Gpu& g : hw.gpus)
        {
            std::string label = std::format("{}: {} ({}, {})", g.index, g.name, g.type == 2 ? "dedicated" : g.type == 1 ? "integrated" : "other",
                FormatBytes(g.vram));
            if (ImGui::RadioButton(label.c_str(), current == std::to_string(g.index)))
                state.Set("gpu", std::to_string(g.index));
        }
        ImGui::SeparatorText("Folders");
        ImGui::Text("Data: %s", dataDir.string().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Open##data")) OpenInFileManager(dataDir);
        ImGui::Text("Logs: %s", (dataDir / "logs").string().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Open##logs")) OpenInFileManager(dataDir / "logs");
        ImGui::Text("Cache: %s", cacheDir.string().c_str());
        ImGui::Spacing();
        if (ImGui::Button("Re-check the game install")) CheckWhere();
        ImGui::SameLine();
        if (ImGui::Button("Rebuild the game for this PC") && gameDir) StartBuild();
        Help("Translates and compiles the game again from your installed default.xex, for this CPU.");
        ImGui::Text("Game program: %s", gameBin.string().c_str());
        ImGui::SameLine();
        if (ImGui::Button("Tune settings for this PC again")) AutoTune(true);
    }
}

int main(int argc, char** argv)
{
    App app;
    app.ResolvePaths();
    app.state.Load(app.dataDir / "launcher.ini");
    DetectSystem(app.hw, app.dataDir.string());
    std::vector<std::string> args(argv + 1, argv + argc);
    bool headlessPlay = std::find(args.begin(), args.end(), "--play") != args.end();
    bool headlessAi = std::find(args.begin(), args.end(), "--build-ai") != args.end();
    // --update: check for a newer release, install it if there is one, then exit.
    bool headlessUpdate = std::find(args.begin(), args.end(), "--update") != args.end();
    bool updateStarted = false;
    // --setup <disc image>: the whole first run without the clicks (build, install, AI textures).
    std::string setupImage;
    if (auto it = std::find(args.begin(), args.end(), "--setup"); it != args.end() && it + 1 != args.end())
        setupImage = *(it + 1);

    std::string error;
    if (!gfx::Init("NFSC Linux", 1100, 700, app.hw, error))
    {
        fprintf(stderr, "[launcher] %s\n", error.c_str());
        // No window: start the game straight away (it says what's wrong itself).
        app.PickGameBinary();
        auto a = app.GameArgv({});
        std::vector<char*> cargs;
        for (auto& s : a) cargs.push_back(s.data());
        cargs.push_back(nullptr);
        execv(cargs[0], cargs.data());
        return 1;
    }
    app.PickGameBinary();
    ImGuiIO& io = ImGui::GetIO();
    float scale = SDL_GetWindowDisplayScale(gfx::Window());
    if (!app.fontPath.empty())
        io.Fonts->AddFontFromFileTTF(app.fontPath.c_str(), 17.0f);
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0;
    style.FrameRounding = 4;
    style.GrabRounding = 4;
    style.ScaleAllSizes(scale > 0 ? scale : 1);
    style.FontScaleDpi = scale > 0 ? scale : 1;
    style.Colors[ImGuiCol_Header] = ImVec4(0.85f, 0.35f, 0.05f, 0.45f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.85f, 0.35f, 0.05f, 0.65f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.55f, 0.15f, 1);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(1.0f, 0.55f, 0.15f, 1);
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(1.0f, 0.55f, 0.15f, 1);

    app.CheckWhere();
    app.LoadSettings();
    // First run on this computer (or other hardware): tune the settings to it.
    if (app.settings.Loaded() && app.state.Get("fingerprint") != app.hw.Fingerprint())
        app.AutoTune(true);
    if (!app.hw.x86_64_v3)
        app.notice = "This CPU lacks AVX2 (x86-64-v3): the game needs an Intel Haswell / AMD Zen or newer CPU.";
    // Updates: GitHub's newest release, each time the launcher starts (System > Updates).
    if (headlessUpdate || (app.CheckAtStart() && setupImage.empty() && !headlessAi && !headlessPlay))
        app.StartUpdateCheck();
    if (!setupImage.empty())
        app.StartSetup(setupImage);
    else if (headlessAi)
        app.StartAiBuild();
    if (headlessPlay)
        app.Play();

    bool quit = false;
    while (!quit)
    {
        SDL_Event e;
        bool any = false;
        while (SDL_PollEvent(&e))
        {
            any = true;
            if (app.capturing && e.type == SDL_EVENT_KEY_DOWN)
            {
                if (const char* token = settings::keynames::Token(uint16_t(e.key.scancode)))
                {
                    app.settings.SetText(*app.capturing, token);
                    app.SaveSettings();
                }
                app.capturing = nullptr;
                continue;
            }
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT)
                quit = true;
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(gfx::Window()))
                quit = true;
        }
        app.HandleDialog();
        app.PollJob();
        app.PollGame();
        app.PollUpdate();
        if (app.restartRequested)
            quit = true;
        if (headlessUpdate && app.upd != App::Upd::Checking && !app.job.active)
        {
            if (app.upd == App::Upd::Available && !updateStarted)
            {
                updateStarted = true;
                app.StartUpdateInstall();
            }
            else
            {
                fprintf(stderr, "[launcher] update: %s\n", app.updateMessage.c_str());
                quit = true;
            }
        }
        if ((headlessAi || !setupImage.empty()) && !app.job.active)
            quit = true;
        if (!gfx::BeginFrame())
        {
            SDL_Delay(50);
            continue;
        }
        app.Ui();
        gfx::EndFrame();
        if (!any && !app.job.active)
            SDL_WaitEventTimeout(nullptr, app.gameRunning ? 500 : 100);
    }
    if (app.job.active)
        app.job.proc.Stop();
    app.SaveSettings();
    gfx::Shutdown();
    // Restart now: the updated AppImage, with the same arguments.
    if (app.restartRequested && !app.appImagePath.empty())
    {
        std::vector<std::string> again = { app.appImagePath };
        again.insert(again.end(), args.begin(), args.end());
        std::vector<char*> cargs;
        for (auto& a : again) cargs.push_back(a.data());
        cargs.push_back(nullptr);
        execv(app.appImagePath.c_str(), cargs.data());
    }
    return 0;
}
