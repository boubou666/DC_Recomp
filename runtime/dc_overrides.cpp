// Game-specific runtime setup for Dark Cloud (NTSC-U, SCUS-97111).

#include "game_overrides.h"
#include "ps2_runtime.h"

#include <iostream>

namespace
{
    constexpr uint32_t kDcNtscEntry = 0x00100008u;

    void applyDc(PS2Runtime &runtime)
    {
        (void)runtime;
        std::cout << "[dc] Dark Cloud (NTSC-U SCUS-97111) overrides applied" << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE("Dark Cloud (USA)", "SCUS_971.11", kDcNtscEntry, 0u, &applyDc)
