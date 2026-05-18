#pragma once

//
// GPIO controller functional quirks
//
#define QUIRK_NONE                      0x00000000
#define QUIRK_ACPI10                    0x00000001
#define QUIRK_ACPI20                    0x00000002
#define QUIRK_NEEDS_RESET_WORKAROUND    0x00000004
#define QUIRK_BROKEN_CLOCK_GATE         0x00000008
#define QUIRK_NO_DMA_SUPPORT            0x00000010
#define QUIRK_SLOW_CLOCK                0x00000020
#define QUIRK_NO_D1D2                   0x00000040


//
// GPIO controller BSOD-workaround quirks
//
#define BSOD_NONE                       0x00000000
#define BSOD_FORCE_PIO                  0x00000001
#define BSOD_EXTRA_RESET                0x00000002
#define BSOD_MASK_INTERRUPTS            0x00000004
#define BSOD_DELAY_INIT                 0x00000008
