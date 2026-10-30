// Game-specific runtime setup for Dark Cloud (NTSC-U, SCUS-97111).

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

namespace
{
    constexpr uint32_t kDcNtscEntry = 0x00100008u;

    // Native frame loop (DC_NATIVE=1, the default in scripts/run.sh): no vblank paces the game. MGEndFrame (0x12D100)
    // ends each frame with WaitVSync(last) (0x12D070), which spins until the vblank handler's counter (gp-0x73F0,
    // 0x2A2400) differs from the count of the previous frame end, then stores the new count: one frame per vblank at
    // most, and the game steps its animations by the vblanks that went by.
    // Here the wait blocks the main thread until the frame's deadline on the PC clock (EeScheduler::waitHostDeadline,
    // the FramePacer cap, 60 by default), as JC2_Recomp does: the game's other threads run meanwhile, EE time is
    // charged with the real time the frame took (the vblanks, which still count on EE time, go on), the frame is
    // shown, and the wait returns to MGEndFrame.
    constexpr uint32_t kWaitVSync = 0x0012D070u;
    constexpr uint32_t kVSyncCount = 0x002A2400u; // gp 0x2A97F0 - 0x73F0
    constexpr double kEeClockHz = 294912000.0;

    std::atomic<uint32_t> g_frames{0};

    void nativeFrameEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t ra = getRegU32(ctx, 31);
        g_frames.fetch_add(1u, std::memory_order_relaxed);
        static auto s_next = std::chrono::steady_clock::now();
        const uint32_t cap = runtime->framePacer().cap();
        const auto now = std::chrono::steady_clock::now();
        s_next = cap == 0u ? now : std::max(now, s_next + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                               std::chrono::duration<double>(1.0 / cap)));
        runtime->eeScheduler().waitHostDeadline(
            s_next,
            [runtime]
            {
                EeScheduler &scheduler = runtime->eeScheduler();
                static uint64_t s_lastFrameCycle = scheduler.currentCycle();
                static auto s_lastFrame = std::chrono::steady_clock::now();
                const auto woke = std::chrono::steady_clock::now();
                const double seconds = std::min(0.1, std::chrono::duration<double>(woke - s_lastFrame).count());
                s_lastFrame = woke;
                const uint64_t accounted = scheduler.currentCycle() - s_lastFrameCycle;
                const uint64_t realCycles = static_cast<uint64_t>(seconds * kEeClockHz);
                if (realCycles > accounted)
                    scheduler.accountCycles(static_cast<uint32_t>(std::min<uint64_t>(realCycles - accounted, 0xFFFFFFFFu)));
                s_lastFrameCycle = scheduler.currentCycle();
                scheduler.presentGuestFrame();
            },
            [ra](R5900Context &resume) { resume.pc = ra; });
    }

    // DC_FPS=1 (and the native loop): frames per second every 5 s, from the frame ends and the vblank counter.
    void fpsThread(PS2Runtime *runtime)
    {
        const uint8_t *rdram = runtime->memory().getRDRAM();
        const auto vblanks = [rdram]
        {
            uint32_t value = 0;
            std::memcpy(&value, rdram + kVSyncCount, sizeof(value));
            return value;
        };
        auto last = std::chrono::steady_clock::now();
        uint32_t lastFrames = g_frames.load(), lastVblanks = vblanks();
        while (!runtime->isStopRequested())
        {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            const auto now = std::chrono::steady_clock::now();
            const uint32_t frames = g_frames.load(), counted = vblanks();
            const double seconds = std::chrono::duration<double>(now - last).count();
            std::fprintf(stderr, "[dc:fps] %.1f fps, %.1f vblanks/s\n", (frames - lastFrames) / seconds,
                         (counted - lastVblanks) / seconds);
            last = now;
            lastFrames = frames;
            lastVblanks = counted;
        }
    }

    void applyDc(PS2Runtime &runtime)
    {
        // Every libmc call of the game passes slot 1 (CMemoryCardAccess::SearchMcType 0x213E7C: sceMcGetInfo(port, 1,
        // ...); LoadSysConfig 0x214360: sceMcOpen(port, 1, ...)). MCSERV without a multitap answers for the card in
        // the port whatever the slot; the runtime's libmc only took slot 0, and the game said "Memory Card (PS2)
        // (8MB) not inserted" at boot.
        ps2_stubs::setMcAnySlot(true);

        const char *native = std::getenv("DC_NATIVE");
        const char *fps = std::getenv("DC_FPS");
        if ((native && native[0] == '1') || (fps && fps[0] == '1'))
            std::thread(fpsThread, &runtime).detach();
        if (native && native[0] == '1')
        {
            runtime.eeScheduler().setNativeFrames(true);
            runtime.registerFunction(kWaitVSync, &nativeFrameEnd);
            std::cout << "[dc] native loop: frames paced by the host (cap " << runtime.framePacer().cap()
                      << " fps, 0 = unlimited)" << std::endl;
        }
        std::cout << "[dc] Dark Cloud (NTSC-U SCUS-97111) overrides applied" << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE("Dark Cloud (USA)", "SCUS_971.11", kDcNtscEntry, 0u, &applyDc)
