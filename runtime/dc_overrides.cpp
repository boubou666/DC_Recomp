// Game-specific runtime setup for Dark Cloud (NTSC-U, SCUS-97111).

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"

#include <iostream>

namespace
{
    constexpr uint32_t kDcNtscEntry = 0x00100008u;

    void applyDc(PS2Runtime &runtime)
    {
        (void)runtime;
        // Every libmc call of the game passes slot 1 (CMemoryCardAccess::SearchMcType 0x213E7C: sceMcGetInfo(port, 1,
        // ...); LoadSysConfig 0x214360: sceMcOpen(port, 1, ...)). MCSERV without a multitap answers for the card in
        // the port whatever the slot; the runtime's libmc only took slot 0, and the game said "Memory Card (PS2)
        // (8MB) not inserted" at boot.
        ps2_stubs::setMcAnySlot(true);
        std::cout << "[dc] Dark Cloud (NTSC-U SCUS-97111) overrides applied" << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE("Dark Cloud (USA)", "SCUS_971.11", kDcNtscEntry, 0u, &applyDc)
