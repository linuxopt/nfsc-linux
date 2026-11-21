// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// NFS: Carbon: unlock every car, part and race (NFSMW_UNLOCK_ALL=1; the
// AppImage launcher's "Unlock all cars", for the player's own copy).
//
// Carbon keeps EA's debug "unlock all things" flag, a byte at 0x82C6503C,
// in the shipped Xbox 360 build. Eighteen functions read it: the unlock
// system's lookup (sub_821E6458: with the flag set, nothing is locked), the
// car lot, the career and the front end's lists (sub_8263FDC0 and others).
// Nothing in the game sets it but a debug path and the profile serializer
// (sub_824775B0, which reads it into a save and writes it back from one).
//
// Downloadable content (the Marketplace car unlocks, the Collector's
// Edition's DLC_CE_TRACKS) goes through the content manager instead (the
// singleton at 0x82C65918, made by sub_826401E0): its first virtual,
// sub_826407E8, is "is this content id unlocked", a search of its list of
// installed content. It doesn't look at the flag, so it answers yes here.
//
// So, with NFSMW_UNLOCK_ALL=1:
//   - the flag is set once the unlock system exists (sub_821E5A58 makes it,
//     at boot) and after each profile load;
//   - the profile serializer runs with the flag at 0, so a save never
//     records it: turning the option off gives the game its own unlocks back;
//   - the content manager's lookup says every content id is installed.
// Without it, none of this changes anything.
#include <stdafx.h>
#include <kernel/function.h>
#include "hidden_content.h"

#include <cstdio>
#include <cstdlib>

namespace
{
    constexpr uint32_t kUnlockAllThings = 0x82C6503C;

    const bool s_unlockAll = [] {
        const char* v = std::getenv("NFSMW_UNLOCK_ALL");
        bool on = v && v[0] == '1';
        if (on)
            fprintf(stderr, "[unlock] NFSMW_UNLOCK_ALL=1: every car, part, race and downloadable content unlocked (saves don't record it)\n");
        return on;
    }();
    // NFSMW_UNLOCK_LOG=1: the first lookups and what they answered.
    const bool s_log = [] { const char* v = std::getenv("NFSMW_UNLOCK_LOG"); return v && v[0] == '1'; }();
    int s_logged = 0;
}

extern "C" PPC_FUNC(__imp__sub_821E5A58);
extern "C" PPC_FUNC(__imp__sub_824775B0);
extern "C" PPC_FUNC(__imp__sub_826407E8);
extern "C" PPC_FUNC(__imp__sub_821E6458);

// The unlock system's lookup (only logged here: the flag does the unlocking).
PPC_FUNC(sub_821E6458)
{
    uint32_t key = ctx.r4.u32;
    __imp__sub_821E6458(ctx, base);
    if (s_log && s_logged < 400)
    {
        s_logged++;
        fprintf(stderr, "[unlock] lookup %08X: %s (flag %u)\n", key, ctx.r3.u32 ? "locked" : "unlocked", PPC_LOAD_U8(kUnlockAllThings));
    }
}

// The unlock system's creation (boot).
PPC_FUNC(sub_821E5A58)
{
    __imp__sub_821E5A58(ctx, base);
    if (s_unlockAll)
        PPC_STORE_U8(kUnlockAllThings, 1);
    hidden::OnBoot(base);  // hidden_content.cpp: the debug menu's flag
}

// The profile serializer: the flag stays out of saves, in either direction.
PPC_FUNC(sub_824775B0)
{
    if (!s_unlockAll)
    {
        __imp__sub_824775B0(ctx, base);
        return;
    }
    PPC_STORE_U8(kUnlockAllThings, 0);
    __imp__sub_824775B0(ctx, base);
    PPC_STORE_U8(kUnlockAllThings, 1);
}

// The content manager's "is this content id unlocked".
PPC_FUNC(sub_826407E8)
{
    uint32_t id = ctx.r4.u32;
    if (s_unlockAll)
        ctx.r3.u64 = 1;
    else
        __imp__sub_826407E8(ctx, base);
    if (s_log && s_logged < 400)
    {
        s_logged++;
        fprintf(stderr, "[unlock] content %08X: %s\n", id, ctx.r3.u32 ? "installed" : "not installed");
    }
}
