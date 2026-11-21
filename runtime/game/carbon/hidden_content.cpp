// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// NFS: Carbon: what the retail Xbox 360 game ships but hides, for the
// player's own copy (the AppImage launcher's Hidden Content options).
//
// NFSC_DEBUG_MENU=1: the main menu's tenth option, CAR(DEBUG). The menu's
//   setup (sub_82642DB0) adds it only while the byte at 0x82C64FEF is set,
//   and nothing in the retail game sets it. It opens EA's debug car screen
//   (UI_DebugCarCustomize.fng): every vehicle in the game, every part slot.
//
// NFSC_EXTRA_CARS=cops,dev: cars the game has but never offers, in Quick
//   Race's car select (its Stock list). The front end's car database (the
//   FEDatabase at 0x82C64AD0, +212, its record array at +564) holds 200
//   20-byte records: +0 a handle (-1: empty), +4 the car's pvehicle key (the
//   attribute hash, Jenkins' lookup2 with 0xABCDEF00, of its name), +8 its
//   preset key (the same for a stock car), +12 its filter bits (the high
//   half the class: 1 muscle, 2 exotic, 4 tuner, 8 bonus; the low half the
//   list: 0x0001 stock), +16 the customization and career slots (0xFF: none).
//   When car select's screen is pushed (sub_822FDB00, the front end's
//   package push) a record is put in an empty slot for each car (handles
//   0x4E46xxxx, ours), and the unlock check for a record (sub_821E71D8: its
//   name's key against the unlock list, which none of these are in) says
//   unlocked for ours. They're taken out again whenever the main menu or the
//   world map is pushed and at boot, so they never hold slots the career
//   needs and no save keeps them. At most kMaxSlots of the empty slots are
//   used, and kKeepFree are always left.
//
// NFSC_FNG_LOG=1: each front-end package pushed (a diagnostic).
#include <stdafx.h>
#include <kernel/function.h>
#include "hidden_content.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr uint32_t kDebugMenuFlag = 0x82C64FEF;
    constexpr uint32_t kFeDatabase = 0x82C64AD0;
    constexpr uint32_t kOurHandle = 0x4E460000;  // "NF"
    constexpr int kRecords = 200, kRecordSize = 20;
    constexpr int kKeepFree = 12;

    bool Env(const char* name)
    {
        const char* v = std::getenv(name);
        return v && v[0] == '1';
    }

    // The game's attribute keys: Bob Jenkins' lookup2, seeded 0xABCDEF00.
    uint32_t AttribKey(std::string_view s)
    {
        auto mix = [](uint32_t& a, uint32_t& b, uint32_t& c) {
            a -= b; a -= c; a ^= c >> 13;
            b -= c; b -= a; b ^= a << 8;
            c -= a; c -= b; c ^= b >> 13;
            a -= b; a -= c; a ^= c >> 12;
            b -= c; b -= a; b ^= a << 16;
            c -= a; c -= b; c ^= b >> 5;
            a -= b; a -= c; a ^= c >> 3;
            b -= c; b -= a; b ^= a << 10;
            c -= a; c -= b; c ^= b >> 15;
        };
        const auto* k = reinterpret_cast<const uint8_t*>(s.data());
        uint32_t len = uint32_t(s.size()), a = 0x9E3779B9, b = 0x9E3779B9, c = 0xABCDEF00;
        while (len >= 12)
        {
            a += k[0] | (k[1] << 8) | (k[2] << 16) | (uint32_t(k[3]) << 24);
            b += k[4] | (k[5] << 8) | (k[6] << 16) | (uint32_t(k[7]) << 24);
            c += k[8] | (k[9] << 8) | (k[10] << 16) | (uint32_t(k[11]) << 24);
            mix(a, b, c);
            k += 12;
            len -= 12;
        }
        c += uint32_t(s.size());
        switch (len)
        {
        case 11: c += uint32_t(k[10]) << 24; [[fallthrough]];
        case 10: c += uint32_t(k[9]) << 16; [[fallthrough]];
        case 9: c += uint32_t(k[8]) << 8; [[fallthrough]];
        case 8: b += uint32_t(k[7]) << 24; [[fallthrough]];
        case 7: b += uint32_t(k[6]) << 16; [[fallthrough]];
        case 6: b += uint32_t(k[5]) << 8; [[fallthrough]];
        case 5: b += k[4]; [[fallthrough]];
        case 4: a += uint32_t(k[3]) << 24; [[fallthrough]];
        case 3: a += uint32_t(k[2]) << 16; [[fallthrough]];
        case 2: a += uint32_t(k[1]) << 8; [[fallthrough]];
        case 1: a += k[0];
        }
        mix(a, b, c);
        return c;
    }

    // The vehicles (pvehicle collections) the game has but never offers.
    // Police: the pursuit cars, the player's cop cars of online Pursuit Tag.
    constexpr const char* kCops[] = {
        "copmidsize", "copghost", "copsport", "copsportghost", "copsporthench", "copgto", "copgtoghost",
        "copsuv", "copsuvl", "copsuvpatrol", "copcross",
        "player_cop", "player_cop_corvette", "player_cop_gto", "player_cop_suv",
    };
    // The developers': test and demo cars, the story's own cars (bosses,
    // crew, Cross), the fire and dump trucks. (Not "911turbo": the unused
    // 911 Turbo S has no model left, the game draws a placeholder.)
    constexpr const char* kDev[] = {
        "demo_vid_gallardo", "temp_exotic_03", "temp_muscle_01", "temp_muscle_02",
        "t1_exotic_neville", "t1_muscle_neville", "t1_tuner_neville", "m3gtrcareerstart", "wrx_red", "carrera_grnp7",
        "hero", "cross", "bmwm3gtre46_crosschase",
        "darius", "darius_ai", "angie", "angie_casino", "kenji", "kenji_casino", "rival_kenji", "wolf", "wolf_casino",
        "colin", "nikki", "samson", "yumi",
        "player_firetruck", "player_dumptruck",
    };

    const std::vector<uint32_t> s_extraCars = [] {
        std::vector<uint32_t> keys;
        const char* v = std::getenv("NFSC_EXTRA_CARS");
        if (!v || !*v)
            return keys;
        std::string list = std::string(",") + v + ",";
        auto add = [&](const auto& names) {
            for (const char* n : names)
                keys.push_back(AttribKey(n));
        };
        if (list.find(",cops,") != std::string::npos)
            add(kCops);
        if (list.find(",dev,") != std::string::npos)
            add(kDev);
        fprintf(stderr, "[hidden] %zu extra cars for Quick Race (%s)\n", keys.size(), v);
        return keys;
    }();

    uint32_t Records(uint8_t* base)
    {
        uint32_t db = PPC_LOAD_U32(kFeDatabase);
        uint32_t p = db ? PPC_LOAD_U32(db + 212) : 0;
        uint32_t array = p ? PPC_LOAD_U32(p) : 0;
        return array ? array + 564 : 0;
    }

    bool Ours(uint32_t handle) { return (handle & 0xFFFF0000) == kOurHandle; }

    void RemoveCars(uint8_t* base)
    {
        uint32_t recs = Records(base);
        int removed = 0;
        for (int i = 0; recs && i < kRecords; i++)
        {
            uint32_t r = recs + i * kRecordSize;
            if (!Ours(PPC_LOAD_U32(r)))
                continue;
            PPC_STORE_U32(r, 0xFFFFFFFF);
            PPC_STORE_U32(r + 4, 0);
            PPC_STORE_U32(r + 8, 0);
            PPC_STORE_U32(r + 12, 0);
            PPC_STORE_U32(r + 16, 0xFFFFAAAA);
            removed++;
        }
        if (removed)
            fprintf(stderr, "[hidden] %d extra cars taken out of the car list\n", removed);
    }

    void AddCars(uint8_t* base)
    {
        uint32_t recs = Records(base);
        if (!recs || s_extraCars.empty())
            return;
        int empty = 0;
        for (int i = 0; i < kRecords; i++)
            empty += PPC_LOAD_U32(recs + i * kRecordSize) == 0xFFFFFFFF;
        int added = 0, budget = empty - kKeepFree;
        for (uint32_t key : s_extraCars)
        {
            bool have = false;
            int slot = -1;
            for (int i = 0; i < kRecords; i++)
            {
                uint32_t r = recs + i * kRecordSize;
                uint32_t handle = PPC_LOAD_U32(r);
                if (handle == 0xFFFFFFFF)
                {
                    if (slot < 0)
                        slot = i;
                }
                else if (Ours(handle) && PPC_LOAD_U32(r + 4) == key)
                    have = true;
            }
            if (have)
                continue;
            if (slot < 0 || added >= budget)
                break;
            uint32_t r = recs + slot * kRecordSize;
            PPC_STORE_U32(r, kOurHandle | uint32_t(slot));
            PPC_STORE_U32(r + 4, key);
            PPC_STORE_U32(r + 8, key);
            PPC_STORE_U32(r + 12, 0x00020001);  // stock (the class shown comes from the car itself)
            PPC_STORE_U32(r + 16, 0xFFFFAAAA);  // no customization, no career slot
            added++;
        }
        if (added)
            fprintf(stderr, "[hidden] %d extra cars in the car list (%d empty slots before)\n", added, empty);
    }

    void OnPackage(uint8_t* base, uint32_t namePtr)
    {
        if (namePtr < 0x82000000u || namePtr > 0x83100000u)
            return;
        char name[64] = {};
        for (int i = 0; i < 63; i++)
            if (!(name[i] = char(PPC_LOAD_U8(namePtr + i))))
                break;
        static const bool s_log = Env("NFSC_FNG_LOG");
        if (s_log)
            fprintf(stderr, "[fng] %s\n", name);
        std::string_view n(name);
        if (n == "FeCarSelect.fng")
            AddCars(base);
        else if (n == "FeMainMenu.fng" || n == "WorldMapMain.fng")
            RemoveCars(base);
    }
}

namespace hidden
{
    void OnBoot(uint8_t* base)
    {
        if (Env("NFSC_DEBUG_MENU"))
        {
            PPC_STORE_U8(kDebugMenuFlag, 1);
            fprintf(stderr, "[hidden] NFSC_DEBUG_MENU=1: CAR(DEBUG) on the main menu\n");
        }
        RemoveCars(base);
    }
}

extern "C" PPC_FUNC(__imp__sub_822FDB00);
extern "C" PPC_FUNC(__imp__sub_821E71D8);

// The front end's package push (r4: the package's name).
PPC_FUNC(sub_822FDB00)
{
    OnPackage(base, ctx.r4.u32);
    __imp__sub_822FDB00(ctx, base);
}

// "Is this car record unlocked" (r4: its handle): ours are.
PPC_FUNC(sub_821E71D8)
{
    if (Ours(ctx.r4.u32))
    {
        ctx.r3.u64 = 1;
        return;
    }
    __imp__sub_821E71D8(ctx, base);
}
