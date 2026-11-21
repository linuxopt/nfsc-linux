// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// NFS: Carbon: content the retail game ships but hides (hidden_content.cpp).
#pragma once
#include <cstdint>

namespace hidden
{
    // At boot, once the unlock system exists (unlock_all.cpp's hook).
    void OnBoot(uint8_t* base);
}
