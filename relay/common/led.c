#include <3ds.h>
#include <string.h>

#include "ftp.h"
#include "led.h"

void led_broadcast(led_broadcast_t mode) {
    MCU_InfoLedPattern p;
    memset(&p, 0, sizeof(p));
    if (mode == LED_BROADCAST_WAIT) {
        /* 32 frames, 2 s: blue rises then falls, interpolated, looping. */
        p.animation.ticksPerFrame = mcuTicksFromMs(62);
        p.animation.frameTransitionDelay = p.animation.ticksPerFrame;
        p.animation.numLastFrameRepeats = 0;
        for (int i = 0; i < 32; i++) p.bluePattern[i] = (u8)(i < 16 ? i * 12 : (31 - i) * 12);
    } else {
        p.animation.ticksPerFrame = 1;
        p.animation.numLastFrameRepeats = 0xFF;
        if (mode == LED_BROADCAST_LIVE) memset(p.bluePattern, 0xC0, sizeof(p.bluePattern));
    }
    if (R_FAILED(mcuHwcInit())) { ftp_log("led: mcu::HWC unavailable"); return; }
    Result r = MCUHWC_SetInfoLedPattern(&p);
    mcuHwcExit();
    if (R_FAILED(r)) ftp_log("led %d: %08lX", mode, r);
}
