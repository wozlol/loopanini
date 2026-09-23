#pragma once

// Hardware and build constants for loopanini. See ../../FIRMWARE_PLAN.md for
// the full picture. Pure macros only: the vendored USB Host Shield library
// includes this file too (see src/USB_Host_Shield_Library_2.0/avrpins.h), so
// nothing Arduino specific may go in here.

// ===========================================================================
// SWITCHES YOU MIGHT WANT TO FLIP. Everything below the next banner is a fact
// about the hardware, read the pin budget before changing any of it.
// ===========================================================================

// USB MIDI host through the stacked Module USB v1.2. 1 = on, 0 = off. When on,
// the module's DIP switches must be set right (middle SS switch ON, first IN
// switch ON, all others OFF, see the USB host section below) and Port A (red)
// must be empty.
#ifndef LOOPANINI_ENABLE_USB_HOST
#define LOOPANINI_ENABLE_USB_HOST 1
#endif

// DIN MIDI through the Unit MIDI. 1 = on, 0 = off.
#ifndef LOOPANINI_ENABLE_DIN_MIDI
#define LOOPANINI_ENABLE_DIN_MIDI 1
#endif

// Which of ModuleAudio's two 3.5mm jacks the Mixer's EXT (aux in) channel
// reads from. 1 = LINPUT1/RINPUT1, the TRS-only jack. 2 = LINPUT2/RINPUT2,
// the TRRS combo jack (also accepts a plain TRS plug). M5Stack's own docs
// disagree with each other on which jack is "the mic" versus "the aux/line"
// jack, unconfirmed on this hardware, see FIRMWARE_PLAN.md's Mixer status
// notes. If EXT sounds hissy, full of the synth, or noisy no matter what's
// plugged in or how low its fader is, try flipping this, the other input may
// be floating (nothing wired to it) and picking up crosstalk.
#ifndef LOOPANINI_AUX_ADC_INPUT
#define LOOPANINI_AUX_ADC_INPUT 2
#endif

// Which port the Unit MIDI is plugged into. Pick LOOPANINI_PORT_B (black, the
// default) or LOOPANINI_PORT_C (blue). Do NOT pick LOOPANINI_PORT_A (red) while
// USB host is on, GPIO1 on that connector is the host's chip select. If you do,
// the boot pin guard refuses DIN MIDI and prints why, nothing gets damaged.
#define LOOPANINI_PORT_A 0
#define LOOPANINI_PORT_B 1
#define LOOPANINI_PORT_C 2
#ifndef LOOPANINI_DIN_MIDI_PORT
#define LOOPANINI_DIN_MIDI_PORT LOOPANINI_PORT_B
#endif

// ===========================================================================
// PIN BUDGET. Read this before changing any GPIO or any DIP switch.
//
// The stack is CoreS3 + ModuleAudio (switch B) + Module USB v1.2, plus a
// Unit MIDI on one of the Grove ports. Everything on the M-Bus shares nets, so two devices
// that pick the same M-Bus pin are electrically wired together whatever the
// firmware says. The table is decoded from M5Unified's CoreS3 M-Bus table
// (M5Unified.cpp) and cross-checked against the Module USB v1.2 datasheet's
// CORES3 row at every switchable pin.
//
//   M-Bus  GPIO  ModuleAudio (B)   Module USB v1.2 DIP        Port A (Grove)
//   pin 2   10                     INT Select CH1
//   pin 19   2                                                SDA (keep Port A empty)
//   pin 20   1                     SS Select CH2              SCL (keep Port A empty)
//   pin 21   6   LRCK
//   pin 22   7   MCLK              SS Select CH1  <-- COLLIDES with MCLK
//   pin 23  13   DOUT
//   pin 24   0   BCK               SS Select CH3  <-- COLLIDES with BCK
//   pin 26  14   DIN               INT Select CH2 <-- COLLIDES with DIN
//   pin  7  37   (SPI MOSI, shared with the CoreS3 LCD and SD by design)
//   pin  9  35   (SPI MISO)
//   pin 11  36   (SPI SCK)
//   pin  4   8   free                                          Port B pin 1 (in)
//   pin 10   9   free                                          Port B pin 2 (out)
//   pin 15  18   free                                          Port C pin 1 (RX)
//   pin 16  17   free                                          Port C pin 2 (TX)
//   (Ports B and C are Grove connectors on the battery bottom. Their pins are
//   M5Unified's own CoreS3 port table, and they line up with the M-Bus pins
//   the Core family wires them to.)
//
// Consequences (derived from the datasheets and M5Unified's pin table, the
// phase 2 symptoms match, confirm on hardware after the guard build):
//  * SS CH1 (GPIO7) puts the MAX3421E chip select on ModuleAudio's master
//    clock. Driving it as a chip select kills the codec's MCLK, and the
//    codec's output turns into bitcrushed noise. Prime suspect for the
//    phase 2 bitcrush noise.
//  * The only DIP setting that avoids ModuleAudio is SS CH2 + INT CH1. That is
//    what the defaults below assume.
//  * SS CH2 is GPIO1, which is also Port A's SCL (the red port). So Port A must
//    stay EMPTY while USB host is on. Anything plugged into it, the Unit MIDI
//    especially, would drive the chip select line.
//  * DIN MIDI (Unit MIDI) therefore lives on Port B (black, GPIO8/9) by
//    default, or Port C (blue, GPIO18/17). ModuleAudio + Module USB + Unit MIDI
//    all coexist as long as the Unit MIDI is not on Port A.
//
// src/pin_guard.h enforces this at boot: every subsystem claims the pins it
// touches, a conflicting subsystem is skipped with a loud message instead of
// being allowed to corrupt the audio clock.
// ===========================================================================

// AMY renders at a fixed 48000 Hz, AMY_SAMPLE_RATE in amy.h. ModuleAudio's
// ES8388 is told to match so no resampling is needed between the two.
#define LOOPANINI_SAMPLE_RATE 48000

// ModuleAudio I2C address and bus speed, matches M5Stack's own examples.
#define LOOPANINI_MODULEAUDIO_I2C_ADDR 0x33
#define LOOPANINI_MODULEAUDIO_I2C_SPEED 400000

// USB audio out (src/usb_audio_out.h) starts at unity, independent of
// ModuleAudio/CoreS3 speaker's analog output level. Both are adjustable
// once the Mixer screen exists, see FIRMWARE_PLAN.md's Mixer section.
#define LOOPANINI_USB_AUDIO_OUT_LEVEL_DEFAULT 1.0f

// ---------------------------------------------------------------------------
// DIN MIDI via the M5Stack Unit MIDI (SAM2695) used as a plain DIN in/out UART
// bridge, its onboard synth chip is deliberately unused. The port it is plugged
// into is chosen by LOOPANINI_DIN_MIDI_PORT at the top of this file. Pin 1 of a
// port is the UART RX and pin 2 is the TX, the same pin order that was
// confirmed working on Port A. A simple opto-isolated DIN breakout on the same
// two pins behaves identically.
//
// NOT Port A while USB host is on, see the pin budget above.
// ---------------------------------------------------------------------------
#define LOOPANINI_DIN_MIDI_BAUD 31250
#define LOOPANINI_DIN_MIDI_UART_NUM 1

// ---------------------------------------------------------------------------
// USB MIDI host via the stacked Module USB v1.2 (MAX3421E over SPI), turned on
// or off by LOOPANINI_ENABLE_USB_HOST at the top of this file. The module's DIP
// switches MUST be: on the SS group (3 switches)
// only the MIDDLE one ON, on the IN/INT group (2 switches) only the FIRST one
// ON, everything else OFF. Never two ON in the same group, that would wire two
// M-Bus pins together. The two GPIO numbers below must match: SS CH2 is GPIO1,
// INT CH1 is GPIO10. Port A (red) must stay empty, GPIO1 is on its connector.
// pin_guard skips host at boot, with a message, if these collide with
// anything already claimed.
// ---------------------------------------------------------------------------
#ifndef LOOPANINI_USB_HOST_CS_GPIO
#define LOOPANINI_USB_HOST_CS_GPIO 1
#endif
#ifndef LOOPANINI_USB_HOST_INT_GPIO
#define LOOPANINI_USB_HOST_INT_GPIO 10
#endif

// The audio and MIDI work runs as one task pinned to core 0, per
// FIRMWARE_PLAN.md's core split. Core 1 is left for the touch UI, which
// there isn't yet.
#define LOOPANINI_AUDIO_TASK_CORE 0
#define LOOPANINI_AUDIO_TASK_PRIORITY 5
#define LOOPANINI_AUDIO_TASK_STACK_BYTES 8192

// USB MIDI host polling has its own task on core 1, off the real-time audio
// core. It is a full host polling and enumeration state machine, not a quick
// bounded check like the other MIDI transports, so it does not belong in the
// audio loop as a matter of design. (Moving it did not change the phase 2
// crash, and neither did raising the stack to 8192, which was a precaution
// that was never shown to be the problem. The prime suspect is the pin
// collision above.)
#define LOOPANINI_USB_HOST_TASK_CORE 1
#define LOOPANINI_USB_HOST_TASK_PRIORITY 1
#define LOOPANINI_USB_HOST_TASK_STACK_BYTES 8192

// Looper arm trigger: peak level (0 to 1) of the looper input that starts a
// recording after ARM. 0.02 is about -34 dBFS. Raise it if noise starts takes.
#ifndef LOOPANINI_LOOPER_ARM_THRESHOLD
#define LOOPANINI_LOOPER_ARM_THRESHOLD 0.02f
#endif

// Longest loop the looper buffer holds, seconds of stereo audio in PSRAM.
#ifndef LOOPANINI_LOOPER_MAX_SECONDS
#define LOOPANINI_LOOPER_MAX_SECONDS 16
#endif

// SD sample cache (channel 10 drums, and single-sample pitched channels).
// See sample_bank.h and FIRMWARE_PLAN.md's Synth and sampler section.
#ifndef LOOPANINI_SD_KIT_DIR
#define LOOPANINI_SD_KIT_DIR "/kits/000"  // auto-loaded at boot if present
#endif
// PCM preset numbers we hand out for SD-loaded samples. Picked well clear of
// the baked-in TR-808 bank (0-10) and AMY's synth patch numbers (0-255 Juno/
// DX7, 256 piano, 1024+ user patches), which are a separate namespace from
// PCM presets but easier to reason about kept apart anyway.
#ifndef LOOPANINI_DRUM_PRESET_BASE
#define LOOPANINI_DRUM_PRESET_BASE 2000  // + MIDI note number, one per drum
#endif
#ifndef LOOPANINI_PITCHED_PRESET_BASE
#define LOOPANINI_PITCHED_PRESET_BASE 3000  // + channel index 0..2, one sample per channel
#endif
// Reserved raw oscillators for round robin drum sample playback, so hits
// don't fight the default Juno/DX7/kit synths for voices. max_oscs is 250 by
// default (amy_default_config), default_synths uses well under 200 of them.
#ifndef LOOPANINI_DRUM_OSC_BASE
#define LOOPANINI_DRUM_OSC_BASE 240
#endif
#ifndef LOOPANINI_DRUM_OSC_COUNT
#define LOOPANINI_DRUM_OSC_COUNT 8  // simultaneous drum hits
#endif
