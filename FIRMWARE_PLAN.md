# Loopanini firmware plan

This is the gospel reference for the Loopanini build. It gets written before code the
same way FIRMWARE_3_PLAN.md did for arpnmidi, because this project has more
simultaneous subsystems than that one did, audio capture, synth render, sample
cache, LEDs, and touch UI all sharing one chip, so the responsibilities and the
looper's state machine need to be nailed down before anything gets typed into
the Arduino IDE.

## MVP milestone (2026-09-25)

Marked here at the user's request as the goalpost for a minimum viable
build: looper (record/overdub/play/undo/clear, time machine mode), AMY
synth and sampler across 4 channels (patches, live SD sample drums, per
channel volume/voices), a 4 level mixer (INT/EXT/LOP/ALL) with a working
SP1LimiterJS style limiter (automatic makeup gain) per channel, beat
stutter on any of the four, USB device/host/DIN MIDI, and a touch UI
across five screens, all confirmed working on real hardware as of the
Mixer section's eighteenth pass status note. Settings persist across
reboots. Not yet in the MVP: per channel effects beyond the limiter, the
Custom/SD Card patch categories, and the tremolo/vibrato/univibe combo
effect, see that same status note's backlog for the full detail on each.

## Hardware

- M5Stack CoreS3: ESP32-S3, dual core Xtensa LX7 at 240 MHz, 8 MB PSRAM, 16 MB
  flash, 2.0 inch capacitive touch IPS display, onboard ES7210 mic array.
- M5Stack ModuleAudio v1.0 (ES8388 codec), switch set to **B**. Configuration A
  is the Core2/Basic I2S pin mapping. CoreS3's onboard ES7210 already occupies
  those default pins, so A will fight it. B is the mapping M5Stack built
  specifically so ModuleAudio and CoreS3's own ES7210 sit on separate pins and
  don't conflict, which is also what makes switching between the external
  ModuleAudio jack and the internal mic a software choice rather than a
  hardware one.
- Battery bottom with WS2812B LEDs, driven for status and beat feedback, not
  as a second display.
- M5Stack Unit MIDI (SAM2695) in **Port B (black)** by default, used as a plain
  DIN MIDI in/out UART bridge at 31,250 baud, its onboard synth chip unused.
  Port B is GPIO8 (RX, the Unit's TX) and GPIO9 (TX). Port C (blue, GPIO18 and
  GPIO17) works the same. Chosen by `LOOPANINI_DIN_MIDI_PORT` in the switches
  block at the very top of `src/config.h`, which also holds the on/off flags
  for DIN MIDI and USB host. **Not Port A (red):** its GPIO1 carries the USB host chip select. DIN MIDI started
  on Port A, where notes were confirmed reaching AMY, and moved when USB host
  took priority. Ports B and C are Grove connectors on the battery bottom. A
  simple opto-isolated DIN breakout on the same two pins would behave
  identically.
- M5Stack Module USB v1.2 (MAX3421E, SPI), stacked, for USB MIDI host only.
  Unlike the older USB Module, v1.2 added DIP switches specifically to
  support CoreS3. MOSI/MISO/SCK are fixed at GPIO37/35/36 on CoreS3, not
  switched, shared with the CoreS3's LCD and SD by design. The chip select
  and INT lines are DIP selected, and **which positions are safe depends on
  what else is on the M-Bus, see Pin budget below.** The correct setting next
  to ModuleAudio: on the SS group (3 switches) only the middle one ON, on the
  IN/INT group (2 switches) only the first one ON, all others OFF, never two
  ON in one group. The switch advice this
  document originally gave (SS Select CH1 + INT Select CH1) was wrong, it
  collides with ModuleAudio. It was stated without checking, and is
  corrected here.

### Pin budget

Everything on the M-Bus shares nets, so two modules that pick the same M-Bus
pin are wired together whatever the firmware says. Decoded from M5Unified's
CoreS3 M-Bus table (`M5Unified.cpp`) and cross-checked against the Module USB
v1.2 datasheet's CORES3 row at every switchable pin (all five agree). ModuleAudio's pins
are what `audio_io.cpp` actually passes to the driver for position B on
CoreS3. `src/config.h` carries the same table next to the flags it drives.

| M-Bus pin | GPIO | ModuleAudio (B) | Module USB v1.2 | Port A |
|---|---|---|---|---|
| 2 | 10 | | INT Select CH1 | |
| 19 | 2 | | | SDA, Unit MIDI RX (our TX) |
| 20 | 1 | | SS Select CH2 | SCL, Unit MIDI TX (our RX) |
| 21 | 6 | LRCK | | |
| 22 | 7 | **MCLK** | **SS Select CH1** | |
| 23 | 13 | DOUT | | |
| 24 | 0 | **BCK** | **SS Select CH3** | |
| 26 | 14 | **DIN** | **INT Select CH2** | |
| 4 | 8 | | | Port B pin 1 (in), free |
| 10 | 9 | | | Port B pin 2 (out), free |
| 15 | 18 | | | Port C pin 1 (RX), free |
| 16 | 17 | | | Port C pin 2 (TX), free |

Consequences:

- **SS Select CH1 (GPIO7) is ModuleAudio's MCLK.** Driving it as a chip select
  removes the codec's master clock, which is the prime suspect for the phase 2
  bitcrush noise. CH3 (GPIO0) collides with BCK, and INT CH2 with DIN.
- The only positions that avoid ModuleAudio are **SS Select CH2 (GPIO1) + INT
  Select CH1 (GPIO10)**.
- GPIO1 is also on Port A's connector (SCL), so **Port A (red) must stay empty**
  while USB host is on. Anything plugged in there, the Unit MIDI especially,
  would drive the chip select line. That is why DIN MIDI moved to Port B.
- With the Unit MIDI on Port C or Port B, **ModuleAudio + Module USB v1.2 + Unit
  MIDI all coexist**, no pins shared. Ports B and C were the answer, an
  earlier version of this section wrongly concluded only two of the three could
  run because it only considered Port A.
- `src/pin_guard.cpp` enforces this at boot. Each subsystem claims its GPIOs
  all-or-nothing, in priority order (audio first), and one that would collide
  is refused with a message naming both owners, instead of being allowed to
  corrupt the audio clock. Unit tested off target, `tests/run_tests.sh`.

## Toolchain and libraries

Arduino IDE stays. The thing that actually breaks performance on ESP32 isn't
the IDE, it's writing everything into one `loop()` so UI work stalls the audio
callback. `arduino-esp32` exposes the same FreeRTOS task pinning
(`xTaskCreatePinnedToCore`) that ESP-IDF does, so the fix is task discipline,
not switching build systems.

Planned libraries:

- **AMY** (`shorepine/amy`, Arduino Library Manager) as the synth and sampler
  foundation. It does I2S output, MIDI note handling with voice stealing, and
  a sampler mode that supports both RAM loaded samples and disk streamed
  samples with base note and loop point support. That split maps directly
  onto the ch10 versus ch1 to 3 requirement below.
- **M5Unified / M5CoreS3** for display, touch, power, and onboard mic access.
- **M5Module-Audio** or a direct ES8388 driver for the external codec.
- **FastLED** or **Adafruit_NeoPixel** for the WS2812B strip.
- **SD** (SDMMC or SPI, whichever CoreS3's wiring uses) for kit storage.
- **USBMIDI** (arduino-esp32's own native class, `USB.h` + `USBMIDI.h`) for
  USB MIDI device mode on CoreS3's native USB-C port. Requires the board's
  "USB Mode" option set to USB-OTG (TinyUSB), not the default Hardware CDC
  and JTAG setting.
- **USBAudioCard** (also arduino-esp32's own native class, `USBAudioCard.h`,
  no separate library install) makes that same native port double as a
  class-compliant, output-only (device to host) 2 channel USB audio
  interface: constructed with `UAC_SPK_NONE, UAC_MIC_STEREO`, so there is no
  USB playback path at all, only loopanini's own mix going up to the host.
  A connected computer sees loopanini as a stereo capture source with no
  driver needed, no analog cable back into a separate audio interface.
  Confirmed by test compile that `USBAudioCard` and `USBMIDI` compose onto
  one physical port as a single USB device with both classes enumerated:
  both classes register themselves, then one shared `USB.begin()` call
  brings the whole composite device up.
- The device shows up to the host OS as "Loopanini," not a generic default,
  via `USB.productName("Loopanini")` before `USB.begin()`, plus `USBMIDI`
  constructed with that same name (`USBMIDI midi("Loopanini")`) since some
  hosts show the MIDI port's own name instead of, or alongside, the overall
  device's product name.
- Board setting gotcha found during phase 1 bring up, corrected after
  hardware testing: with USB Mode set to USB-OTG (TinyUSB), `Serial`
  defaults to UART0 on physical pins, not the USB-C cable, so the Serial
  Monitor shows nothing no matter what the sketch prints. The first fix
  tried, USB CDC On Boot set to Enabled so `Serial` maps to the USB CDC
  interface, compiles fine but is wrong in practice: that board option
  makes the core auto-start a CDC-only USB connection before `setup()`
  even runs (main.cpp's `ARDUINO_USB_CDC_ON_BOOT` block), and this
  sketch's own code registering MIDI and Audio afterward then forces a
  disruptive re-enumeration in the middle of boot, which killed the
  Serial Monitor connection at exactly the moment USB MIDI came up,
  confirmed on real hardware. **Correct fix: USB CDC On Boot stays
  Disabled**, and the sketch owns its own `USBCDC` object (`src/debug_io.h`),
  registered alongside `USBMIDI` and `USBAudioCard` before the one shared
  `USB.begin()` call, so there is exactly one enumeration with the full
  composite descriptor from the start, not an early CDC-only one that MIDI
  and Audio have to force their way into after the fact.
- **USB Host Shield Library 2.0**, vendored and patched, not the global
  Library Manager copy, for USB MIDI host, driving the stacked Module USB
  v1.2 (MAX3421E) over SPI. Chip select and INT are `LOOPANINI_USB_HOST_CS_GPIO`
  and `_INT_GPIO` in `src/config.h`, default GPIO1 (SS Select CH2) and GPIO10
  (INT Select CH1), the only pair that avoids ModuleAudio, see Pin budget.
  Four files patched (pins from `config.h`, bounded hardware waits, see
  `patches/README.md`). This keeps the native USB-C port free to stay in device mode
  while still getting host input from a class-compliant USB MIDI keyboard
  through the module. Talks to the `USBH_MIDI` class the library ships
  directly (`src/midi_host.cpp`), not the separate UHS2-MIDI wrapper
  library, since UHS2-MIDI's own headers use angle-bracket includes that
  would silently reach past the vendored copy back to the global one,
  defeating the point of vendoring. Same approach arpnmidi already uses for
  its own MAX3421E code, and its `patches/README.md` pattern is mirrored
  here (`patches/README.md`, `loopanini/src/USB_Host_Shield_Library_2.0/`)
  for the same reason: the global copy stays untouched and stock, so a
  Library Manager update can never silently break this pin binding, and
  there's a durable, reviewable record of exactly what changed and why.
  Verified by temporarily hiding the global copy and confirming the sketch
  still builds (exit 0) from the vendored copy alone.
- **Arduino's own `HardwareSerial`** for DIN MIDI over Port A (CoreS3's only
  externally exposed Grove port), talking to the M5Stack Unit MIDI
  (SAM2695) as a plain UART passthrough, its onboard synth chip unused.
  Port A's two signal pins (`m5::pin_name_t::ex_i2c_sda`/`ex_i2c_scl`) are
  looked up through M5Unified rather than hardcoded, same pattern as
  ModuleAudio's pins. A simple opto-isolated DIN breakout, if it replaces
  the Unit MIDI later, would use those same two pins the same way.
- **AMY-Arduino.h**'s own `convert_midi_bytes_to_messages()` is the common
  landing point for all three MIDI sources (USB device, USB host, DIN). Each
  transport is parsed by its own library, not AMY's built-in MIDI transport
  code, and the resulting bytes are handed to AMY's parser directly. AMY's
  own `AMY_MIDI_IS_USB_GADGET` transport path only compiles when
  `AMYBOARD_ARDUINO` is defined, which pulls in board-specific macros that
  don't exist for a generic CoreS3 target, so it's not used here.

## Core and task split

- **Core 0**: I2S read (looper capture), AMY render and I2S write, MIDI
  parsing for USB device MIDI and DIN MIDI specifically, since both are
  quick bounded checks (a non-blocking packet read, a UART `available()`
  check), arm and threshold detection, loop transport and quantize to bar
  boundary. Everything here is bounded and non blocking. No SD reads and no
  flash writes happen on this core during active recording or playback.
- **Core 1**: touchscreen rendering, touch input and swipe detection, WS2812B
  updates, SD directory scans and kit loading into PSRAM, and USB MIDI host
  polling (Module USB v1.2). Kit loading is chunked across multiple passes
  so it never stalls audio on core 0, even though it isn't running on that
  core, because it still has to hand finished sample buffers over safely.
  USB MIDI host polling lives here rather than on core 0 as a design rule:
  USB Host Shield's `Usb.Task()` is a full host polling and enumeration
  state machine, not a quick bounded check the way the other MIDI
  transports are, so it doesn't belong in the real-time audio loop. It runs
  as its own task on core 1, and feeds AMY the same way the other
  transports do, `amy_add_event`'s own source comments confirm it's safe to
  call from any sending thread, not just one designated producer, so this
  isn't a data race, it's what AMY's event queue is designed for. The task
  ends itself if the feature is disabled or its pins were refused.


**Module USB v1.2 switch labels (verified against the official table, 2026-09-19):** the labels printed on the module are the original Core row. Physical switches are the same on every host. SS: G13 (CoreS3 G7, clash), G5 (CoreS3 G1, USE), G0 (CoreS3 G0, clash). INT: G35 (CoreS3 G10, USE), G34 (CoreS3 G14, clash). On a CoreS3 with ModuleAudio, turn ON only the switch labelled G5 and the one labelled G35.

### Phase 2 crash, what was and wasn't the cause

**Found 2026-09-19, decoded from a real panic (verified, not a hypothesis):** the USB host task crashed in `USB::Task()` at `devConfig[i]->Poll()` with LoadProhibited (a null or garbage pointer). The USB Host Shield library never zeroes `USB::devConfig[]` and relies on the `USB` object being a zero-filled global, as in all its examples. We created it with `new`, and heap memory is not zeroed. Fix: `midi_host.cpp` now constructs it with placement new into static (zero-filled) storage. Boot now prints a `PANIC task=... backtrace:` line from the saved core dump, decode with `xtensa-esp32s3-elf-addr2line -pfiaC -e loopanini.ino.elf <addresses>` against the exact build that was flashed. **Confirmed on hardware 2026-09-19:** no crash (host MIDI input from a real keyboard is NOT yet working or verified, earlier note claiming it played was wrong, the notes came from the USB device port), all three STAGE lines OK, DIN on Port B running, write failures 0. Note the heartbeat line is per 2 seconds, so about 188 blocks per line is the healthy rate (the "~375" label is stale).

The phase 2 build (USB host + DIN added) produced bitcrush noise, 100%
ModuleAudio write failures, self reboots, and a follow-on "ModuleAudio not
found." The record, corrected. An earlier version of this document blamed an
undersized 4096-byte host task stack for memory corruption. **That was never
demonstrated and is withdrawn:** the crash was identical after moving the task
to core 1 and again after raising its stack to 8192, so neither was the cause.
Ruled out with data: CoreS3 uses quad, not octal, PSRAM (`CONFIG_SPIRAM_CLK_IO`
/`CS_IO` are GPIO30/26, nowhere near the module's pins), and M5GFX's display
uses `SPI3_HOST` while the host library's default `SPI` object is `SPI2_HOST`,
different peripherals (the pads are shared by design, see Pin budget).

What the evidence supports, still to be confirmed on hardware:

- **Pin collision.** The chip select was set to GPIO7, ModuleAudio's MCLK. See
  Pin budget. This matches "noise that tracks the notes" (AMY's data was fine,
  the codec's clock was not) and is why phase 1, without the host, was clean.
- **Reboots are a core 0 hot loop.** The build has `CONFIG_ESP_TASK_WDT_TIMEOUT_S 5`
  with `CHECK_IDLE_TASK_CPU0` on and CPU1 off. The block rate in the logs
  (~1850 blocks/s against a healthy 375) shows `I2S.write()` was failing
  instantly instead of blocking on the clock, and nothing else in the audio
  loop waits, so it spun at full speed, starved core 0's idle task, and the
  watchdog rebooted the board. A core 1 hang would not have rebooted anything.
  The audio task now yields (`vTaskDelay(1)`) on a failed write.
- **Why the writes failed instantly is not established.** `i2s_channel_write`
  only returns fast on an invalid channel state, and nothing in our code
  disables the channel. The staged bring up and the ESP-IDF error log hook
  below exist to catch the actual error next time.
- **Unbounded hardware waits** in the vendored USB host library (`Init()`
  spinning on `SAMPLEBUS`, `OutTransfer()` spinning on the transfer-done IRQ)
  would hang a core if the SPI link returned garbage. Bounded to 50 ms,
  `patches/README.md`.
- **Debug prints could stall the audio task.** `USBCDC::write()` busy-waits
  up to 250 ms, without yielding, when a terminal has the port open but isn't
  draining it. The CDC TX timeout is now 0, so a write that can't proceed is
  dropped.

### Diagnostics built into the firmware

- **Staged bring up.** ModuleAudio + USB MIDI start alone for 2 s, then DIN
  MIDI is enabled, then USB host, with a health check after each: a stage is
  OK only with zero write failures and a block rate within 20% of real time.
  The log names the stage that breaks the audio.
- **Boot summary** (reset reason, pin table, stage results). Reset reason names
  watchdog, panic, and brownout resets, so "it rebooted itself" now says why.
  Reprint any time by sending `?` from the Serial Monitor.
- **ESP-IDF error logs routed to the USB serial port**, rate limited to 20
  lines a second. The board's log level is compiled at ERROR only, so this is
  just the driver errors, which is exactly what was invisible before.
- **`b` in the Serial Monitor reboots into the USB bootloader**, so uploads no
  longer need the RESET button hold. `r` resets.
- `tests/run_tests.sh` builds the pin guard against stubs and runs it on the
  host with no hardware. Mutation checked: breaking the guard on purpose makes
  it fail on the phase 2 scenario.

## Audio path

- ModuleAudio (ES8388, switch B) is the default input and output. Bring up
  bug found and fixed: `M5ModuleAudio::begin()`'s sample_rate argument only
  configures the ESP32 side of the I2S bus, the ES8388 codec's own ADC/DAC
  clock dividers are separate registers `es8388->init()` never touches, so
  `setSampleRate(SAMPLE_RATE_48K)` must be called again right after `begin()`
  or the codec runs its conversion clock at its own reset default, mismatched
  against the I2S bus timing, producing no usable audio even though `begin()`
  itself reports success.
- Second bring up bug found: `es8388->init()` (inside `begin()`) sets the
  analog Lout/Rout volume registers to a conservative "-45dB" power-up
  default, and neither this code nor M5Stack's own reference example ever
  explicitly raises it again or confirms the mute bit is clear. Diagnosed
  from real hardware data: MIDI parsing correct, ModuleAudio I2S writes
  succeeding with zero failures, AMY's startup bleep (which plays
  independent of MIDI) still inaudible, pointing at the codec's own output
  stage rather than anything upstream. Fixed with an explicit
  `setSpeakerVolume(100)`, `setSpeakerOutput(DAC_OUTPUT_ALL)` (both output
  channels rather than just one, so this doesn't depend on knowing which
  physical path the jack is wired to), and `setMute(false)`.
- CoreS3's onboard ES7210 mic is available as an alternate input, selectable
  in settings. Because B keeps the two codecs on separate I2S pins, this is a
  software toggle, not a rewiring job.
- CoreS3 also has its own onboard output, a 1 W speaker driven by an AW88298
  I2S amp, entirely separate from ModuleAudio's ES8388 DAC. Output routing
  gets a setting: ModuleAudio out, CoreS3's own speaker, or CoreS3's speaker
  automatically when nothing is connected to ModuleAudio's jack. The manual
  toggle between the two is straightforward, they're independent I2S output
  paths, no shared hardware to fight over. The automatic no-cable-connected
  case depends on ModuleAudio actually exposing a jack detect signal, which
  isn't confirmed yet, see Open Items. If it isn't available, this ships as
  a manual setting only, which was already fine, neither the internal
  speaker nor the internal mic being available is essential.
- AMY's rendered output and the looper's playback buffer are mixed before the
  DAC write. The looper's capture path taps the raw ADC input directly, it
  does not go through AMY, so recording and synth playback are not competing
  for the same buffer.
- A separate setting controls whether the looper's recording input is the
  analog input (Ext), AMY's internal render (Int), or both summed (Both).
  Default is Ext. The arm and threshold logic below is specific to the Ext
  analog path, since Int captures a clean MIDI triggered signal that doesn't
  need noise gating, a note on can just start the take immediately when Int
  or Both is selected.

## Looper

This is the flagship feature and the part that has to be exactly right.

### Modes and timing

- Length mode: bar count in 4/4 or 3/4, or Free.
- BPM: internal, or follow USB MIDI clock, or follow DIN MIDI clock. This is
  a source picker, not a blend, same shape as arpnmidi's Clock Input setting.
- Overdub setting: when a take ends (auto or manual, see below), either roll
  straight into overdub, or drop into plain playback and wait for the
  performer to re-arm.
- There is one shared tempo and bar grid for the whole instrument, not a
  separate one per bank slot. It comes from the BPM and meter setting
  (internal or MIDI follow) when one is active, or gets established by
  whichever loop is recorded first in Free mode. The live loop and all four
  bank slots are always recorded against that same grid, so anything
  bar-locked always lines up with anything else bar-locked, no per-slot
  time-stretching needed or planned. Whether that grid is currently
  "live," meaning something is actually audible on it right now, matters
  for the arm behavior below and for how bank slots start, see those
  sections.

### Arm, pre-roll, and the analog threshold

- Tapping the main control when idle does not start recording, it arms.
  What happens next depends on whether the shared grid is currently live,
  meaning the live loop or at least one bank slot is audibly playing right
  now.
- **Grid live**: skip audio-start detection entirely. The new take starts
  exactly on the next bar boundary of whatever is already playing, the same
  way an incoming MIDI Start would trigger an armed take. There's already a
  reliable, audible reference, so there's no reason to guess the start point
  from input amplitude, and no pre-roll is needed either, since the capture
  window opens on a fixed clock instant rather than gating on a detected
  onset, there's nothing for a gate to clip.
- **Grid not live**: fall back to audio-start detection. While armed, the
  looper is continuously capturing into a short rolling pre-roll buffer and
  watching the Ext input level, ignoring it until it sustains above a
  threshold for a short hold window, so a single noise spike or chair
  scrape can't false trigger it.
- When the sustained threshold is crossed, the take actually starts, but its
  first audio comes from slightly before that crossing, pulled out of the
  pre-roll buffer, so the recorded loop doesn't have a clipped attack pop at
  the trigger point.
- The threshold level, the hold time, and the pre-roll length start as
  constants at the top of the source rather than a UI control, because a
  reasonable default is easy to guess and doesn't need a screen to tune.
  If bring up testing shows it's finicky across different input levels or
  rooms, it gets promoted to a real setting on the looper settings screen.
  Either way it lives in the same settings struct from day one so that
  promotion is a small change, not a rework.
- This entire arm and threshold path only applies when the loop's recording
  input is Ext. Int and Both start on the first note on with no gating.

### Ending a take

- Bar count mode, auto end (the bar count is reached): the take finalizes.
  Then it follows the overdub setting, either straight into overdub or into
  plain playback.
- Bar count mode, manual end before the bar count is reached: the take is
  discarded entirely and the looper re-arms immediately for a fresh attempt.
  This is deliberately not the same as a normal end, ending early in a fixed
  length take means you didn't want that take.
- Free mode, manual end: there is no bar boundary to compare against, so a
  manual end here is the equivalent of auto end. It finalizes the take at
  whatever length was just played and establishes that as the loop length,
  then follows the overdub setting exactly like a bar count auto end would.

### Overdub, replace, and re-arming

- Re-arming a loop that is currently playing and has not been cleared drops
  straight into overdub onto the existing content.
- Clear is always temporary and non destructive by itself. Arming after a
  clear is still just waiting, not recording, so the cleared take remains
  fully recoverable through undo the whole time the looper sits armed.
  Nothing is actually lost until new audio is captured.
- Replace only happens at the moment a new take is actually recorded over a
  cleared loop. That capture is what discards the old generation for good,
  arming by itself never does.
- Exactly one generation is kept, the most recently cleared take, and
  nothing older. This matches arpnmidi's model, a cleared track keeps its
  undo material only until the first new capture replaces it.
- Undo brings the most recently cleared take back and resumes playing it.
  Undo stays available all the way through idle and armed states, and is
  only lost once a new recording actually starts writing audio.

### Controls, split the arpnmidi way

arpnmidi's default button behavior is good and has a lot of precedent behind
it, but it was built for four physical buttons that can be chorded together.
Loopanini is one track on a touchscreen, so buttons that could never
usefully be pressed at once don't need to be separate, and destructive
actions get pulled off the control you tap constantly:

- **Arm / Play / Rec**: one large control, its label and color are
  context sensitive to state (idle, armed and waiting, recording,
  overdubbing, playing). Single tap does the context appropriate thing.
  A double tap on this same control is the fast path, arpnmidi's safe
  clear and arm in one motion, adopted directly: it clears the current loop
  and arms it in the same gesture, so getting from a full loop to ready for
  a new take is one to two taps, never more.
- **Stop / Clear / Undo**: a separate cluster, physically apart from the
  main control so a fast tap on one can't land on the other. Stop pauses
  playback in place without clearing, tap again to resume, a plain
  play/stop toggle when nothing else is going on. Tapping again while
  stopped clears. A small Undo affordance appears in this cluster only when
  there is something to undo, and disappears once nothing can be recovered,
  which is the dynamic, state dependent button behavior arpnmidi uses for
  its looper controls.
- A persistent loop state icon, filled circle recording, open circle armed
  and waiting, triangle playing, short bars stopped with content, hollow
  marker for cleared, borrowed straight from arpnmidi's convention since it
  already reads at a glance.

### Loop banks

Four slot buttons on the main screen sit alongside the live loop's own
Arm/Play/Rec control, letting content graduate out of the working loop into
its own independent player:

- Tapping an empty slot bounces whatever is currently in the live loop
  (playing or stopped with content) into that slot and clears the live loop
  in the same motion, freeing it to arm a new take right away. Banking is
  disabled while the live loop is empty or armed, there is nothing to bank
  yet.
- If the live loop is playing when it's bounced, playback continues without
  interruption, the slot picks up the same read position the live loop was
  already at rather than muting and restarting. It's a handoff, not a stop
  and relaunch. If the live loop was stopped (not playing) when bounced, the
  slot just receives the buffer in the same stopped state.
- Once banked, a slot is a simple independent play and stop toggle. Stop is
  immediate. Start depends on whether the shared grid, see Modes and
  Timing, is currently live:
  - Nothing bar-locked is currently playing anywhere: there's no grid to
    snap to, so tapping starts the slot immediately, free running, and
    since that slot was itself recorded at a bar start, it re-establishes
    the grid's phase from that instant.
  - Something bar-locked is already playing, live loop or another slot:
    tapping schedules that slot to start exactly on the next bar boundary
    of the grid already running, since these were recorded at bar start
    and belong on it.
  - This applies per slot independently, all four can be running or
    stopped in any combination, starting or stopping one doesn't touch the
    live loop's transport or arm state.
- The same grid-live logic governs a brand new recording too: if a bank
  slot is currently playing when the live loop arms, the new take starts on
  the next bar boundary of that running grid rather than waiting for audio
  onset, see Arm, pre-roll, and the analog threshold.
- A banked slot's audio mixes into the master output the same way the live
  loop's playback does, it rides the same Mixer playback level for now
  rather than getting four separate faders, see Mixer.
- Clearing a filled slot to make room for a new bank is a hold on the slot.
- Four slots plus the live loop buffer all need to fit in PSRAM alongside
  the drum sample cache, see Open Items.

## Touch UI

**Status 2026-09-21 (second pass, builds, not yet run on hardware):** mixer redone (INT EXT LOP ALL, full height faders with round handles, round Mute Solo Limiter Rec buttons, LOP has no rec button since it would feed back, all faders default to full, unity gain at the top). Looper core is real now (`src/looper.cpp`, runs in the audio task): ARM waits for the input to pass a threshold (`LOOPANINI_LOOPER_ARM_THRESHOLD` in config.h), records the INT synth for Measures at the BPM, plays back through LOP, overdubs, Time Machine capture with gap, CLEAR then UNDO. Loop audio lives in PSRAM, 16 s max. Not done: loop slots, pre-roll, EXT input, limiters, pump compressor, stutter audio. AMY patch and volume from the AMY screen now send events, MIDI channel change does not. LCD and Module USB share the SPI pads, `src/spi_lock.h` serializes them.

Draft from the 2026-09-21 UX pass. Five screens, always in this order:
**1 Mixer, 2 AMY synth, 3 Looper, 4 Stutter, 5 Config.**

### Navigation
- Bottom touch band: tap or swipe the left side to go to the previous screen,
  the right side to go to the next. Wraps or stops at the ends, decide when
  built.
- **Confirmed (hardware and public docs):** the touch area is 320 x 280, the
  display is 320 x 240, so the bottom 40 px (y 240 to 279) is a touch only
  strip below the glass. Nav taps and swipes live in that strip, nothing is
  drawn there, so no on screen prev and next buttons are needed.
- Every edit or sub screen has the same **X in the upper left** to go back.
- Labels are abbreviated so buttons stay big. Use M5Unified and M5GFX drawing,
  no LVGL for the first version. Use known working snippets and examples for
  sliders, meters, number pads and scrolling, do not invent widgets.

### Shared widgets
- **Parameter list menu** (used by Config, AMY edit screens and others): rows
  of name and value, tap a row to edit. Scroll bar on the right with up and down
  buttons. The bar is split into equal segments, one per page (3 pages = 3
  segments), tap a segment to jump to that page.
- **Value editor** (opens from a tapped row): full screen number pad, plus up
  and down buttons, plus a live value readout, X to go back. Used for BPM,
  Measures, synth parameters and channel numbers. BPM also gets a tap tempo
  button.

### 1. Mixer
Four equal vertical columns, left to right:
`AMY synth master out`, `Aux in monitor`, `Looper output`, `Main out`
(USB and aux audio out).

Each column has:
- A skinny tall touch slider with a vertical level meter. The peak leaves a
  held mark near the top for a moment, like a classic VU or peak hold.
- To the right of the slider, three stacked buttons, equally spaced, close to
  the slider: **top = Mute**, **middle = Limiter ceiling**, **bottom = loop
  record enable** (a loop arrow symbol, on means this channel is recorded into
  the looper).
- Middle button cycles the limiter Threshold 0, -1, -2, -3, -4, -5, -6 dB and
  back to 0. The limiter is always on.
  **Built, using SP1LimiterJS "Simple Peak-1 Limiter" (Michael Gruhn 2006,
  LOSER pack, Samelot/Reaper's Effects/LOSER folder), fetched and ported
  line for line, see the Mixer section's 2026-09-24 tenth pass status
  note for why this replaced the MGA_JSLimiter this project tried first.**
  Real source: `thresh = exp(slider1/8.65617025)`, a peak envelope smoothed
  by a one pole ~10Hz lowpass (`b = -exp(-2*pi*10/srate)`) combined with the
  instant per-sample peak via `max()` for instant-attack/smooth-release,
  `gain = (rms > thresh) ? rms : thresh`, `output = input / gain`. Below
  threshold that divides by a fixed ratio under 1.0, i.e. automatic makeup
  gain toward 0dBFS, which is the behavior this project actually wanted:
  turning the Threshold down should make things louder up to the ceiling,
  not just quieter above it. MGA never had that, it only ever reduces gain.
  Above threshold, dividing by the envelope self bounds the output to
  exactly unity by construction (whichever channel is loudest lands at the
  ceiling), no separate final clamp needed mathematically, kept anyway as
  float/int16 boundary insurance. It is a third party contribution with an
  acknowledgement requirement, not GPL, keep the author credit if ported
  closely, see the header comment in the fetched source.
- **Column 4 (Main out) bottom button is not a loop toggle.** Main must never
  feed back into the looper input. That button turns on the **pumping
  compressor**: fast attack, slow release, release length equal to one beat at
  the current BPM, giving a classic NY style pump that a loud kick can drive.
  Not a literal sidechain, it works off the signal itself. Specify the
  detector, ratio and depth when built, and tie release to BPM (internal or
  MIDI clock).
- Signal path detail (where the loop record taps sit, and the aux monitor
  versus recorded aux) is settled in the Mixer section.

### 2. AMY synth
- Summary page: a 2x2 grid of channels **1, 2, 3, 10**. Each cell shows the
  instrument or sample name and volume. Tap a cell to edit.
- Edit page uses the parameter list menu: synth engine or type, its
  parameters, MIDI channel, volume. Number entry uses the value editor.
- The best parameter layout for AMY is not yet known. Read AMY's patch and
  oscillator parameters and design pages from what it really exposes, then
  revisit.

### 3. Looper
A 4x2 grid. Top row: the four snapshot slots (loop banks). Bottom row, left to
right: **BPM, Measures, Stop, Play**.
- **BPM** opens the value editor (number pad, up and down, tap tempo).
- **Measures** shows 1 to 8, tap opens the value editor.
- **Stop button** changes role with state: playing = STOP, stopped = CLEAR,
  cleared = UNDO.
- **Play button** changes role with state:
  - Normal mode: playing = OVERDUB toggle, stopped = PLAY, cleared = ARM,
    armed = REC NOW. ARM waits and starts recording when it hears audio. REC NOW
    starts recording immediately.
  - Time machine mode: playing = OVERDUB toggle, stopped = PLAY, cleared =
    CAPTURE. CAPTURE saves audio that already happened, so the loop is taken
    from the always running rolling buffer. If the buffer is silent or not
    long enough yet, ignore the press and show a red indication.
- **Time machine gap (0 to 4 beats):** start the captured loop earlier so it
  ends just before the moment the Play button was pressed, by that many beats.
- Time machine mode changes the Play button pattern as above, it is a Config
  setting.

### 4. Stutter
See Beat stutter. A grid of beat divisions. Press and hold a division to
activate it, release to stop. Target (looper, AMY synth or main) is a setting.

### 5. Config
Uses the parameter list menu. Settings so far:
- Time signature: 4/4 or 3/4.
- ~~Limiter release (ms)~~ removed: SP1LimiterJS (the limiter actually built,
  see the Mixer section) hardcodes a fixed ~10Hz envelope lowpass, no
  separate release control the way MGA_JSLimiter (tried first) had one.
- Looper auto overdub: on or off.
- Looper time machine mode: on or off.
- Time machine gap: 0 to 4 beats.
- Audio out: USB, aux, or both.
- SD recording: on or off (setting only for now, implemented late).
- BPM auto from MIDI clock: on or off.
- Record start position setting sits at the top of the list.
- Candidates not yet decided: input source (Ext, Int, Both), mic source,
  threshold, limiter and compressor defaults, screen brightness, MIDI channel
  routing, factory reset.

### Open UI questions
- What Play and Stop show while armed or during pre-roll besides the labels.
- Screen order wraparound.
- Exact stutter division list and layout (see Beat stutter).

## Beat stutter

A dedicated screen with one button per beat division, reached by the same
bottom swipe as the other screens. Pressing a division does an audio version of
the Stutter feature in arpnmidi: the audio repeats a short slice at that
division, locked to the tempo, for as long as the button is held, then
playback continues normally when it is released.

- Buttons cover the whole set of beat divisions, straight, triplet and dotted
  values from a whole note down to the shortest useful one. The exact list is
  settled when the screen is built, sized so each button is easy to hit.
- A setting picks where the stutter is applied: the **looper out**, the
  **AMY synth out**, or the **main out** (the final mix). It is a per-stutter
  target so it can chop just the loop, just the synth, or everything.
- Timing follows the looper tempo and clock (internal BPM or MIDI clock), so
  slices land on the grid. When there is no tempo (free length loops with no
  clock) the stutter needs a fallback tempo, decide when it is built.
- Implementation notes for later: it needs a small capture ring per target so
  the repeated slice is the audio that just played, and it sits in the mixer
  chain at the tap points above (before the target's level for looper and
  synth, at the final sum for main). Model the behavior on arpnmidi's Stutter,
  read its code when this phase starts rather than guessing.

## BLE MIDI host

Later and not essential: act as a BLE MIDI host so the device can connect to a
BLE MIDI controller. Only if there are enough resources left (RAM, CPU on the
core not running audio, and no audio dropouts, the Bluetooth stack is heavy on
an ESP32-S3 that is already running audio, USB host and the looper). Do this
after everything else works, and drop it without regret if it costs stability.
Parse incoming BLE MIDI packets and feed them to AMY through the same path as
USB and DIN MIDI.

## Mixer

Four levels: live looper input monitoring, looper playback, internal synth
and sampler, and USB audio out (see Audio path and Toolchain), each
independent. USB out can be left matching the analog out level for a simple
setup, but doesn't have to be, someone recording over USB and monitoring
through the analog output at the same time may well want those two at
different levels. Touch drag vertical faders, using the actual screen space
rather than tiny numeric steppers.

### Status 2026-09-23 (third pass): boot splash, EXT default, aux hiss/bleed report

A "LOOPANINI / starting..." splash draws the instant `M5.begin()` makes the
display usable, in `loopanini.ino`, well before ModuleAudio, AMY, SD kit
loading and USB host bring up finish, so the several second boot doesn't look
like a hang. `ui::begin()`'s first real draw replaces it once setup finishes.
EXT now defaults to 75%, not full, since it's a live analog input and this
hardware can put a hot signal on it (see below). Slider fader handle geometry
fixed: it had been centered 1px into its sprite, clipping its left edge, the
sprite is now 1px wider on the right instead of shifting the handle, so it no
longer clips on either side.

**Hardware audio report from the first live aux test, not resolved yet:** a
bassy oscillation ("flappy") when EXT and Main are both near full, AMY's synth
audibly bleeding into the EXT channel as a hiss at full apparent volume (while
turning INT itself all the way up stays quiet and clean by comparison), and
plugged-in aux audio sounding grumbly, stacked and choppy despite being the
right pitch. All three read like some mix of acoustic/electrical feedback
(the aux mic/line hearing the speaker) and analog crosstalk on ModuleAudio's
input, not a rendering or timing bug, since the reported pitch is correct.
Two problems investigating this: M5Stack's own docs disagree with each other
on which of ModuleAudio's two 3.5mm jacks (`ADC_INPUT_LINPUT1_RINPUT1` vs
`ADC_INPUT_LINPUT2_RINPUT2`) is "mic" versus "aux/line", one docs page calls
LINPUT1 the TRS-only jack and LINPUT2 the TRRS combo jack that also accepts a
plain TRS plug, a second source claimed the opposite, and neither is
confirmed against this actual board. `LOOPANINI_AUX_ADC_INPUT` in `config.h`
(1 or 2) now picks which input EXT reads from, so this is a one line change
and reflash to test, no code hunting, if the currently selected jack turns
out to be the wrong or a floating one (a floating high impedance ADC input is
exactly the kind of thing that would pick up crosstalk as a hiss). Open
questions for the next hardware session: which physical jack is the aux
source actually plugged into, is that jack physically or acoustically close
to the speaker (would explain the oscillation and the bleed directly), and
does the problem persist with the speaker output muted or through headphones
instead (isolates acoustic feedback from an electrical/firmware cause).

**Update 2026-09-23: ruled out acoustic feedback and checked the actual code
paths, not guessing this time.** The user confirmed the aux source is a
powered speaker setup and its mic element does not pick up voice, so this is
not a mic-hears-speaker acoustic loop. Traced the real signal path instead of
speculating further:
- `ui::processBlock()`'s `aux` parameter is `const int16_t *`, read only.
  `extSig[]` (the EXT channel) is written purely from `aux[i] * g1`, nowhere
  does AMY's `b[]` (the INT buffer) write into `extSig[]`. There is no digital
  code path from AMY into the aux channel.
- `stutter::apply(b, frames, T_SYNTH)` returns immediately without touching
  `b[]` whenever T_SYNTH isn't the selected stutter target (the default
  target is Main), so it isn't quietly attenuating or otherwise touching the
  INT signal either.
- The ES8388 driver (`Module-Audio/src/es8388.cpp`) explicitly disables
  analog line bypass for both channels at `init()`
  (`DACCONTROL17`/`DACCONTROL20 = 0x90`, bit 6 clear, the driver's own
  comment says so), and `audio_io.cpp` never calls `setLineBypass()` or
  `setMixSourceSelect()` to turn it back on. So the codec's own documented
  analog monitor/bypass path is off, confirmed by reading both the driver and
  our own call sites, not assumed.
- DAC output volume is maxed by `setSpeakerVolume(100)` (works out to the
  driver's top digital DAC volume register value), so INT sounding quiet
  relative to EXT is not an unintended digital attenuation on the INT path.

With the obvious code-level explanations checked and ruled out, what's left
is analog behavior below the code: crosstalk between the DAC output and ADC
input on ModuleAudio, picked up at the ADC's input pins before digitization
and then amplified by the mic preamp gain we apply to the whole aux path
(`MIC_GAIN_12DB`, a substantial analog gain stage, applied to whatever is
actually present at the selected input pins, crosstalk included). This fits
every symptom: EXT hearing AMY as hissy and comparatively loud (crosstalk
boosted by the preamp), a real plugged in aux signal arriving "grumbly,
stacking, choppy" at the right pitch (the real signal plus the same crosstalk
layered on top), and the bassy oscillation when EXT and Main are both high
(two correlated copies of the same signal summing and reinforcing each
other, not necessarily acoustic feedback, an analog crosstalk loop between
DAC and ADC can motorboat the same way).

Added a second switch to test this directly: `LOOPANINI_AUX_MIC_GAIN_DB` in
`config.h` (0/3/6/9/12/15/18/21/24, default dropped to **0dB** from the
driver's own 12dB example, since a typical line level aux source doesn't
need mic preamp gain at all). If the hiss and bleed scale down with this
gain, that confirms analog crosstalk amplified by the preamp rather than
anything upstream in code.

**Update 2026-09-23 (fourth pass): mic gain test result, and a full digital
trace turned up nothing, new leading theory is a connector short, not
crosstalk.** Hardware test result: dropping `LOOPANINI_AUX_MIC_GAIN_DB` from
12 to 0 changed nothing, AMY still bleeds into EXT at full apparent volume.
That falsifies the preamp-amplified-crosstalk theory above, a real crosstalk
signal riding into the ADC ahead of the preamp would have dropped by 12dB
with the gain. Went back through every remaining code layer between AMY's
output and the aux capture buffer, all the way down to the driver internals
this time, not stopping at our own source:
- `ES8388::init()`'s output mixer register (`DACCONTROL16`) is left `0x00`,
  meaning nothing from the ADC/line-in side is mixed into the DAC output
  either, confirmed by reading `setMixSourceSelect()`'s register map
  (`MIXLIN1`/`MIXLIN2`/`MIXADC`/`MIXRES` options), which we never call.
- `setADCInput()` / `es_adc_input_t` is a plain two-way physical jack mux
  (`0x00` or `0x10` into `ADCCONTROL2`), there is no DAC-loopback or monitor
  option in that register at all, so the codec itself has no register-level
  path for its own DAC output to reappear on the ADC capture.
- The ESP32 core's I2S driver (`ESP_I2S.cpp`) allocates genuinely separate
  `tx_chan`/`rx_chan` handles (`i2s_new_channel`), and `readBytes()`/`write()`
  use separate transform buffers, no shared scratch buffer between them.
- Our own `aux_block` in `loopanini.ino` is `static`, correctly
  `memset` to zero on a failed `readBlock()`, not a stack array that could
  hold stale/aliased contents from another function.
No code path survived this pass. Between the digital mixing code, the codec
register map in both directions, and the I2S driver internals, there is
nowhere left in software for AMY's signal to reach the aux capture buffer.

**New leading theory: a TRS plug in ModuleAudio's TRRS jack.** Per the first
pass above, `LOOPANINI_AUX_ADC_INPUT`'s default (input 2) is documented as
the TRRS combo jack, which a 3-conductor TRS cable can physically bridge two
contacts on (a 3-pole plug's sleeve contact spans what a 4-pole jack exposes
as two separate contacts). If ModuleAudio's line/speaker output and its mic
input share that jack's contacts the way phone-headset TRRS jacks
conventionally do, a plain TRS aux cable would electrically short output
onto input at the connector itself, upstream of and parallel to the preamp,
which is exactly why the gain test had zero effect. This also fits the bassy
"flappy" oscillation with EXT and Main both high (a real analog feedback
loop through that short) and the grumbly/choppy real aux audio (a clean
signal plus a partial short degrading it). Not yet confirmed on hardware,
next test is switching `LOOPANINI_AUX_ADC_INPUT` to 1 (the TRS-only jack,
no shared contacts to short) and/or trying a genuine 4-pole TRRS cable in
jack 2 instead of a 3-pole TRS one.

**Update 2026-09-24 (fifth pass): bleed confirmed fixed by switching jacks,
both ModuleAudio jacks are documented mic inputs, not stereo line inputs,
and AMY's quiet output traced clean.** On real hardware, switching
`LOOPANINI_AUX_ADC_INPUT` to 1 fixed the AMY-into-EXT bleed completely,
confirming the connector-short theory above rather than crosstalk.

Remaining aux symptom, now isolated: constant pops/crackles on EXT (scale
with the EXT fader, so they're in the captured samples, not downstream
noise), and a stereo test source only audible on the left channel while the
pops hit both. Module-Audio's own README states the hardware fact directly,
no more guessing needed: "one TRS jack for microphone input only, one TRRS
jack for both microphone input and headphone output", with CTIA/OMTP
auto-switching for headset mic compatibility. Both of ModuleAudio's jacks
are mic inputs by design, not general stereo line/aux inputs. A stereo
line-level source's right channel landing on a jack conductor the hardware
expects to carry mic bias, not audio, fits both symptoms: silence-or-noise
on that channel (pops) and no real right-channel signal (left-only). This
looks like a hardware input-type mismatch, not a firmware bug. Open
question for the next hardware session: does jack 2 (also mic, but wired
for a full CTIA/OMTP headset) show the same mono-plus-pops behavior with
the same stereo source, which would confirm neither jack does genuine
stereo line-in, versus a per-jack difference.

AMY's own output level: traced the full gain chain end to end this pass,
`ui::processBlock()`'s mixer math (`gLevel[0]^2` at fader default 1.0, no
attenuation), AMY's own `synth_level` and bus `volume` (both documented
default 1.0 in `docs/api.md`, and never actually pushed as events at boot
since `ui.cpp`'s `amySeen[]` change detection starts equal to `amy[]`'s
defaults, though that only matters where they'd otherwise differ), and
ModuleAudio's DAC volume (`setSpeakerVolume(100)`, maxed). Nothing in our
code or AMY's own defaults attenuates it. Most likely explanation left is
the patch/velocity actually being played rather than a hidden gain bug,
not yet confirmed on hardware, worth checking against a different patch or
higher velocity before treating this as a firmware issue.

**Update 2026-09-24 (sixth pass): looked up the real hardware spec instead
of inferring from README wording, added Int/Ext Max Gain Db, and finally
wired the limiter up to the MGA_JSLimiter design this section already
spec'd out.** M5Stack's own product page for this module states it plainly,
no more inferring from README phrasing: **"2-channel mic input, 1-channel
stereo headphone output."** Two independent mono mic paths in, one stereo
pair out, that's the whole input capability of this module, on either jack,
confirmed rather than guessed. `audio_io::readBlock()` now folds left into
right on every captured frame right after `device.record()` succeeds, so
the mixer, looper and stutter all see clean mono-as-stereo instead of
RIN1's floating-pin noise. That's a firmware-side accommodation of a real
hardware ceiling, not a bug fix pretending this module does stereo line-in,
it doesn't.

AMY's quiet output: added `Int Max Gain Db` (default 12) and `Ext Max Gain
Db` (default 0, no change from a plain unity aux passthrough) to Config.
These add linear gain on top of the existing quadratic fader taper, so a
full INT fader now reaches +12dB by default instead of stopping at unity,
giving the meter somewhere to go. This only made sense once the limiter
this section already researched (MGA_JSLimiter, the "Better candidate
found" note above) was actually wired into the audio path, unity headroom
with no limiter is just a louder way to clip, so that's built now too:
`ui.cpp`'s `applyLimiter()`, one instance per mixer channel (`limSt[4]`),
using this section's own spec exactly, two overlapping `srate/128` sample
peak hold windows (implemented as a running window plus the previous
window's frozen peak, equivalent without needing two phase counters),
instant attack, one pole release `exp(-3 / (srate * max(release, 0.05)))`
off the shared `Lim Rel Ms` setting, gain `ceiling / env` above the ceiling.
Runs after each channel's stutter stage (INT, EXT, LOP, then Main last),
so `limiterIdx[c]`'s Mixer button is no longer UI only, and is also what
makes pushing `Int Max Gain Db` above 0 safe rather than just louder
clipping. Not yet confirmed on hardware. The pumping compressor on Main is
still UI only, `compOn` in `ui.cpp`, that's separate work.

**Update 2026-09-24 (seventh pass): the sixth pass limiter had a real bug,
fixed, and the looper stays stereo.** First hardware test after the sixth
pass surfaced it directly: loud AMY chords clipped/glitched instead of
being limited, on a random single speaker rather than both, changing a
channel's limiter ceiling had no audible effect, and pressing Main's
limiter button crashed the board. Root cause, found by re-reading the code
that had just been written rather than guessing at hardware causes: the
sixth pass called `applyLimiter()` as a separate pass *after* the existing
per-sample loop that computes `s0 = b[i] * g0` (fader times
`cfgIntMaxGainDb`'s linear gain) and hard clamps it straight to int16
range. With max gain above 0dB that clamp is a real hard clip, happening
*before* the limiter ever saw the sample, so the limiter had nothing left
to react to, explaining all four symptoms at once (clip instead of limit,
no effect from the ceiling setting, and an asymmetric clip/glitch on
genuinely panned AMY content since the two channels can peak at different
times). Main's own sum step had the identical pattern (sum of three
channels hard clamped before `*g3`, before its limiter pass), which is the
likely crash, though that exact code path no longer exists to confirm
against.

Fixed by restructuring `ui::processBlock()` so INT, EXT and Main run their
limiter inline, on the float value right after the fader/gain multiply and
*before* any int16 cast (`limiterStep()`, shared by a new inline call site
and by `applyLimiter()`, kept only for LOP, which never exceeds unity and
so never had this problem). Signal order is now fader/sum -> limiter ->
stutter for all four channels, consistent, LOP's limiter call moved before
its stutter call to match. Not yet confirmed on hardware.

Also asked whether the looper should go mono, since EXT and AMY both
looked mono from the chord glitch. Traced AMY's own docs before answering:
every oscillator has a `pan` control (`Q`/`pan_coefs`, 0..1, default
centered) and Juno patches specifically drive AMY's chorus effect for
width, so AMY is not mono, the glitch was the bug above. Looper stays
stereo.

**Update 2026-09-24 (eighth pass): the pops likely aren't a loudness
problem at all, added real CPU load measurement instead of guessing
further.** New hardware clues after the seventh pass fix: glitching gets
much worse beyond ~5 held notes, a single held note still pops (silence
never does), aux still pops a lot, and the limiter is not audibly doing
anything even pushed hard. That combination doesn't fit an amplitude/
clipping problem, a limiter cannot fix a dropout, it only controls
loudness, so if these are real-time buffer underruns (a block not ready in
time), the limiter would correctly have zero effect on them, that is
itself a real clue pointing at timing, not gain staging.

Traced whether AMY has real load measurement rather than estimating: it
does, `amy_get_render_load()` and a built in overload failsafe
(`amy_overload_check()`, `config.overload_threshold` default 98% for
250ms, `amy_start()` already wires the threshold in), but that check only
ever runs from AMY's own `i2s.c` platform render loop, which this project
does not use, ModuleAudio owns I2S here through `audio_io.cpp` instead. So
the load tracker has been silently reading 0 the entire project, and the
failsafe has never been armed. Fixed in `loopanini.ino`'s `audioTask()`:
times `synth_engine::renderBlock()` and calls `amy_overload_check()` every
block (arms the failsafe as a side effect, a controlled silence-and-reset
under sustained 98%+ load beats an unpredictable pop or a hang), and times
the full block (render + mixer/limiter/stutter + I2S read/write) too. Both
now print in the existing once a second Serial report: `amy render load
%`, worst render time, worst full block time, against the real budget
(`AMY_BLOCK_US`). Confirmed `AMY_BLOCK_SIZE` is 256 samples (48kHz, not
overridden), a 5333us budget per block, matching the report line's
existing "~375 blocks/s" figure.

Also confirmed `LOOPANINI_DRUM_OSC_BASE` (240) + `LOOPANINI_DRUM_OSC_COUNT`
(8) sit inside AMY's real `max_oscs` default while checking this, read
directly from `amy_default_config()` in `api.c`: 250, not the 180 that
`docs/api.md`'s table currently says, the docs table is stale, the source
is authoritative. Not an out of bounds bug, ruled out concretely rather
than left as an open question.

Not yet confirmed on hardware which of AMY's render cost, this project's
own mixer/limiter/stutter cost, or something else (task priorities, other
tasks stealing core 0 time) is actually the bottleneck, the new numbers
should show that directly on the next test rather than needing another
guess. `num_voices` is hardcoded to 6 in `synth_engine::setPatch()`,
deliberately not raised yet, if the real problem is CPU time rather than
voice allocation, more voices would make it worse, not better.

**Update 2026-09-24 (ninth pass): first real load numbers, they point away
from CPU/timing, added per channel Num Voices, and fixed a stale debug
line found along the way.** First hardware numbers with the eighth pass
instrumentation, "fan" sounding glitching past 3 held notes: amy render
load 13-60% scaling with note count as expected, worst render up to
~4957us (under the 5333us budget even at the worst single block seen),
blocks/s held steady at ~187-189 throughout, matching real time. Worst
block time ran ~7000-8200us regardless of render load, 13% or 60% alike,
so it isn't scaling with polyphony either, most likely just the cost of
two blocking full duplex I2S transfers (read then write) in one loop
iteration rather than a sign of falling behind, blocks/s staying healthy
across every sample supports that reading. Also fixed in passing: the
report line's "healthy ~375" was simply wrong, a stale assumption from a
128 sample block this project has never built with, `AMY_BLOCK_SIZE` has
always been 256 (`~187` is correct, `stageCheck()` nearby already computed
this correctly, only the per second report line hardcoded the wrong
number).

None of that points at a CPU/timing dropout at the polyphony levels
tested so far, which is a real finding, not a null result: it means "fan
sounding" glitching starting past 3 notes is more likely a synthesis or
gain staging issue than a missed deadline. Two candidates worth testing
directly rather than guessing further: AMY's own bus mixdown not auto
compensating for active voice count, so summed voices can overload the
bus internally before our mixer ever sees them (would get worse with
`Int Max Gain Db` pushed above 0, which this project added two passes
ago), or the Juno patch's own chorus effect (docs/juno_patches.md) simply
becoming more audible/modulated sounding with more voices stacked through
it, which is not a bug at all. Cheapest next test: set `Int Max Gain Db`
back to 0 and replay the same 3+ note chord, if the glitch goes away or
is much smaller, that implicates bus level summing made worse by the
gain boost rather than anything AMY does by default.

Also added, since it was asked for directly and is independently useful
for this investigation either way: `Voices` (1-16, mono to AMY's normal
polyphony) is now a real per channel setting on the AMY edit screen next
to Channel/Patch/Volume (`ui.cpp`'s `AmyCh.voices`, `synth_engine::
setVoices()`), not just a hardcoded 6. Lets testing polyphony thresholds
directly rather than only by how many keys are held.

**Update 2026-09-24 (tenth pass): a real, sourced -6dBFS bug found in AMY
itself, and the limiter swapped for the one with automatic makeup gain.**
Gain settings ruled out as the cause of the 3+ note glitch: hardware test
showed `Int Max Gain Db` at 0 changed nothing, and the artifact was
described as genuine hard digital clipping, not chorus, present even at
low velocity, ruling out both "bus summing overload made worse by our
gain boost" and "it's just the chorus" from the ninth pass. That plus "the
limiter doesn't get any louder when turned down" pointed at the limiter
itself rather than gain staging, so it was time to actually read AMY's own
output stage instead of continuing to guess at our own code.

Found `amy_fill_buffer()` (amy.c) halves every sample right after its own
soft clipper on any `ESP_PLATFORM` build: `uintval >>= 1`, comment "For
some reason, have to drop a bit to stop hard wrapping on esp?". Checked
this isn't just an old comment nobody revisited: it is a real, currently
open, maintainer filed bug, shorepine/amy#1169 ("ESP32 and Teensy 4.x
output is halved (-6 dBFS ceiling) by an unexplained `>>= 1` after the
soft clipper"), filed 2026-09-17, with a companion PR #1170 merged
2026-09-22 that only excludes ESP32-P4, explicitly leaving Xtensa ESP32/S3
(this board) still halved. The issue's own history traces the shift to a
2023 port from Tulip, notes the current `i2s_std` 32 bit slot driver this
project also uses is byte-for-byte the same conversion RP2040/RP2350 use
*without* the shift and without wrapping, and that the shift "survived the
driver migration without being re-tested". So every AMY sound on this
board has been rendering at a hard -6dBFS ceiling this entire project,
independent of patch, velocity or any volume setting, that is likely a
real part of why AMY sounded quiet from the start.

Fixed in `synth_engine.cpp`'s `renderBlock()`: doubles AMY's output with a
clamp, undoing the halving in this project's own code rather than hand
patching the vendored library (fragile against a Library Manager update,
and would affect every other sketch using it, not just this one). Not
proven whether this alone explains the hard-edge clipping at low velocity,
still not confirmed on hardware, but it is a real, independently valuable
fix either way, and the AMY issue itself raises the possibility that
whatever "hard wrapping" the shift was guarding against is where a
polyphony-correlated glitch could genuinely live, worth watching for after
this fix lands.

Separately, asked to actually research the other LOSER limiter (the one
with automatic makeup gain, "auto turns up to compensate for the turn
down") rather than keep MGA_JSLimiter, which only reduces gain and was
correctly called out as not audibly doing anything. Fetched the real
SP1LimiterJS source (this section always meant to use this one first, see
the original bullet above) and ported it line for line, replacing
MGA_JSLimiter everywhere in `ui.cpp`: `limiterStep()`/`limiterCoefs()`/
`applyLimiter()`, `LimiterState` down to a single one pole filter state.
Caught and fixed a real bug in the first port attempt before it shipped:
scaled the threshold up to int16 magnitude to match the signal, which
made `gain` come out in the thousands and crushed everything toward
silence instead of riding it up to the threshold, the source's own
`thresh`/`rms`/`gain` all live in its native -1..1 normalized domain, not
int16 magnitude, fixed by normalizing peak/rms inside `limiterStep`
instead. `Int Max Gain Db`/`Ext Max Gain Db` (ninth pass) and `Lim Rel Ms`
(original spec, an MGA-only concept, SP1 hardcodes a fixed ~10Hz envelope
lowpass with no separate release control) are removed from Config, the
first pair is redundant with SP1's own automatic makeup gain and the
second no longer controls anything. Not yet confirmed on hardware.

**Update 2026-09-24 (eleventh pass): this was never a gain/DSP problem,
it's a real-time dropout, tenth pass's fixes did not help.** Hardware
test after the tenth pass: "just as bad... chopped like square wave
tremolo," a reboot with enough notes, crackle that gets worse just from
touching the screen or changing screens, and present "even at low
volumes, its totally gain independant like waveform gaps." That last
point rules out every gain/limiter/DSP theory from every prior pass at
once: a limiter or a gain stage can attenuate or distort a sample, it
cannot blank one out. Literal gaps in the waveform, independent of level,
are a dropped or late audio block, this project's own words for it
("getting interrupted somewhere") match the evidence. `Int Max Gain Db` /
`Ext Max Gain Db` (ninth pass, removed tenth pass) are back, default 12
dropped to 6, direct feedback that removing them made things sound worse,
not just redundant with SP1 as reasoned at the time, they stack with SP1's
makeup gain rather than replacing it.

Checked, ruled out concretely rather than guessed: `M5ModuleAudio::
record(uint8_t*,int)` / `play(const uint8_t*,int)`, the exact overloads
`audio_io.cpp` calls, are pure `I2S.readBytes()`/`I2S.write()`, no I2C
traffic in that path at all, so a shared I2C bus with the touch controller
is not the mechanism. The touch/screen-nav correlation plus the reboot
under load both still point at something stalling core 0 (the audio task)
from core 1 (UI, and USB host MIDI polling, both core 1 per config.h),
most likely at the interrupt or cache level rather than FreeRTOS task
scheduling, since the two are already correctly pinned to separate cores,
that isolation only holds for task scheduling, not for hardware level
stalls like a flash cache disable that both cores share regardless of
pinning. Module USB (MAX3421E) is SPI, sharing a bus with the LCD by this
project's own design (`spi_lock.h`), and runs a continuous polling task on
core 1, unconfirmed whether that specific bus is implicated or whether
this is closer to the flash/cache mechanism, needs a real test rather
than more reading.

Also wired AMY's own overload failsafe hook (`amy_config.
amy_external_overload_hook`, armed since the ninth pass's `amy_overload_
check()` call but never actually logged anywhere visible: AMY's own
message for it goes to plain `stderr`, not the USB CDC port `debug_io.h`
uses) to print through `debug_io` when it fires, `synth_engine.cpp`'s
`onOverload()`. If AMY's failsafe is what is causing the repeated silence
plus reset, this line will say so directly and unambiguously on the next
test, if it does not appear at all while the tremolo is happening, that
rules AMY's own failsafe out and points more firmly at the I2S/task
timing layer this project owns.

Two cheap tests queued for the next hardware session rather than another
guessed code change: does the tremolo/crackle happen at all with
`LOOPANINI_ENABLE_USB_HOST` set to 0 (isolates Module USB's continuous
SPI polling as the cause), and does it happen with the screen simply left
alone, untouched, at the same polyphony (isolates the UI/touch side
specifically from USB host). Not yet confirmed on hardware.

**Update 2026-09-24 (twelfth pass): found and fixed the Mixer screen's
part of it, the USB host test cleared USB host of the "over 4 poly"
symptom specifically.** Both queued tests came back with real, precise
data instead of a plain yes/no: same symptoms with USB host disabled and
USB device MIDI used instead, which rules out Module USB's SPI polling as
the cause of the polyphony linked glitch specifically, that one is
screen independent and transport independent, still unexplained, most
likely genuine render/mix CPU cost now that gain staging is ruled out.

But a second, much more precise observation landed alongside it: aux's
big pops only happen on the Mixer screen, specifically when aux is
unmuted, and stop when it is muted. That pinpoints it, muting a channel
does not change any SPI traffic, it only changes whether that channel's
share of a general dropout is audible (silence times a gap is still
silence), so this reads as "the Mixer screen causes a general dropout,
and you can only hear it on whichever channel has real signal running,"
not "muting aux specifically prevents something."

Read `ui.cpp`'s `tick()`: whenever idling on the Mixer screen, it was
redrawing and `pushSprite()`-ing all 4 meter columns unconditionally, on
a 40ms timer, real signal movement or not: up to 100 SPI transactions a
second just from sitting on that screen, on top of whatever touch
elsewhere adds, going through the shared LCD/Module USB `spi_lock`. No
other screen does anything like this, they only redraw on an actual state
change (`dirty`), which fits why this project's own testing kept landing
back on the Mixer screen specifically. `audio_io.cpp`'s I2S calls carry no
SPI at all (checked, not guessed, see the eleventh pass note), so this
isn't shared-bus contention with the audio driver itself, more likely
either the shared `spi_lock` extending how long the LCD holds the bus, or
a lower level DMA/cache collision between the display's SPI DMA and I2S's
own DMA that doesn't care about task pinning. Either way, less SPI
traffic here directly reduces the collision window regardless of which
exact layer it is.

Fixed: the Mixer meter redraw is now throttled to 10/s (was 25/s) and
skips any column whose displayed level, fader position, or mute state
has not moved enough to look different, rather than re-pushing a pixel
identical sprite over SPI. Not a fix for the screen independent "over 4
poly" symptom, that one still needs real data: watch the render load line
(now with the overload hook logging audibly and visibly if AMY's own
failsafe is what's firing) as poly crosses 4, on whichever screen and
MIDI transport reproduces it most reliably. Not yet confirmed on
hardware.

**Update 2026-09-24 (thirteenth pass): confirmed the Mixer fix (no more
pops anywhere), and found why "worst mix" was so large.** Twelfth pass's
SPI throttle confirmed fixed on hardware, aux is clean. New data for the
still open "4+ poly" symptom: blocks/s genuinely dropping under load (188
down to 180 against a healthy ~187) is real evidence of falling behind,
not just perception, but "worst mix" (`ui::processBlock`'s own time, not
AMY's) was 2664-3616us, climbing with load, which is far more than that
function's actual arithmetic should ever cost on this core; that pointed
at the function itself, not just AMY's render.

Found it: `limiterGain` (called `limiterStep` before this pass) ran once
per FRAME per channel, and calls `sqrtf()` once each time, for the SP1
envelope's `sqrt(lowpass(peak))` term (see the tenth pass note). Once per
frame per channel across INT/EXT/Main inline plus LOP's `applyLimiter()`
is 4 `sqrtf()` calls per frame, times 256 frames, is 1024 `sqrtf()` calls
every single block, a scale of call this function's actual DSP need never
called for. Restructured all four call sites to a two pass form: a cheap
peak-only scan (no sqrt, no filter state) across the whole block, one
`limiterGain()` call using that block's peak, then a flat divide across
every sample, 4 `sqrtf()` calls a block instead of 1024. This quantizes
the envelope's smoothed component to block granularity (~5.3ms). Stated
here at the time as "well under the SP1 filter's own ~100ms time
constant", that number was wrong, corrected in the fourteenth pass below:
a 10Hz one pole's actual time constant is ~15.9ms (tau = 1/(2*pi*10)),
100ms is that corner frequency's period, a different quantity, likely
bled in from the old MGA release default (200ms) this project no longer
uses. The conclusion (block granularity is fine here) still holds, see
below for why.

Not fully explained: why "worst mix" scaled up with note count at all,
since this function's operation count never depended on active oscillator
count, only block size. Most likely cache effects from AMY's own render
touching more memory as more oscillators render, leaving `processBlock`'s
own buffers colder, though not confirmed, this is offered as the honest
leading guess, not a traced fact the way the call count above is. Not yet
confirmed on hardware whether this closes the "4+ poly" symptom or only
shrinks it, next test's render load numbers will show directly.

**Update 2026-09-24 (fourteenth pass): better on hardware, but a new wall
at 6 voices ("flutter"), and a direct question about whether block
granularity (5.3ms) is actually the right polling rate.** Worth answering
precisely rather than just reassuring: the two things in `limiterGain()`
have different timing needs and only one of them cares about polling
rate at all.
- The smoothed component (`st.t`, the one pole "envelope filter") has a
  ~15.9ms time constant (`tau = 1/(2*pi*10)` for a 10Hz corner, corrected
  above), not the ~100ms this section said last pass. For that component
  alone, updating every 5.3ms versus every ~16ms would sound close to
  identical, a lowpass filter cannot "see" input changes much faster than
  its own time constant regardless of how often you feed it.
- The instant attack component (`max(smoothed, peak)`) is different: this
  project's `limiterGain()` is handed that whole block's own true peak
  before gaining that same block, so there is no added latency beyond the
  block itself, a loud sample anywhere in a block is already accounted
  for in the gain applied to that same block. Stretching the poll interval
  past one block (say, every 2-3 blocks) would start to cost real attack
  accuracy: a transient in a skipped block would be gained using a stale
  peak from an earlier block instead of its own, i.e. it could get
  through under-limited. 5.3ms is not an arbitrary default here, it is
  the natural size where "the data we already have in hand" and "fast
  enough to catch a transient in the block it happens in" are the same
  thing for free.
- Net: block granularity (5.3ms) is the right choice, not a cost to poll
  less often for. It also is not a meaningful cost any more regardless,
  the fix that mattered was call COUNT (1024 to 4 a block), not rate; 4
  `sqrtf()` calls a block is close to free on this core.

So the 6 voice wall is most likely not `limiterGain()` any more. Leading
candidates, not yet confirmed: genuine AMY render cost (Juno style voices
use ~5 oscillators each per synth.md, so 6 voices on one channel is ~30
active oscillators, a real, unavoidable compute cost, not a bug), or the
still unexplained cache effect from the thirteenth pass. Next step is the
same render load line, specifically watching whether "worst render" (AMY)
or "worst mix" (this project's own code) is what grows at the 6 voice
wall, that tells them apart. Not yet confirmed on hardware.

**Update 2026-09-24 (fifteenth pass): confirmed, with real data and a
real upstream GitHub issue, this is genuine AMY render cost at 6 voice
chords, already partly optimized upstream, likely a practical ceiling
rather than a bug.** Fresh hardware numbers at the 6 voice wall settle
the "worst render vs worst mix" question directly: worst render hit
5297us (99% of the entire 5333us block budget, by AMY's synthesis alone)
at 55% smoothed render load, worst mix stayed flat around 1700-2200us
the whole time, both quiet and loud. That confirms the thirteenth pass's
`sqrtf()` fix actually worked (`processBlock` is no longer scaling with
load) and confirms the remaining cost is squarely AMY's own render, not
this project's code.

Decoded patch 0 (`patches.h`, "Juno A11 Brass Set 1") directly rather
than trusting the generic ~5 osc/voice figure from synth.md: its wire
string references `v0` through `v5`, 6 oscillators per voice, not 5, so 6
voices is ~36 active oscillators for that one channel, plus it carries a
filter (`F...`) and, per the patch table's own convention, the Juno
chorus. Searched shorepine/amy's issues for prior art rather than
guessing further: **issue #779, "BillieJeanScheduled has dropouts on
AMYboard... when the chords come in"**, root caused in PR #780 to
`amp_combine_controls()` doing "a `powf(10,x)` plus up to ~8 `log2f`
calls per audible oscillator, per render block", with the exact line **"A
6-voice chord (+ bass + drums) multiplies that into a render overrun"**,
same threshold this project independently landed on. Follow up issue #783
flagged sibling hot paths with the same shape (`freq_of_logfreq()`'s
`exp2f`, `filter_process()`'s `cosf`/`sinf`, both called per oscillator
per block, uncached).

Checked our installed copy (`library.properties`: 1.2.171) rather than
assuming it's current: all three fixes are already there, `amy.h`'s
`#pragma GCC optimize ("O2")` (the `-Os`-by-default Arduino build was
half of #779's root cause), `amp_combine_controls()`'s zero-coef skip,
and both `freq_of_logfreq()` and `filters.c`'s biquad generators now call
`exp2_lut()`/`cos2pi()`/`sin2pi()` with the raw `exp2f`/`cosf`/`sinf`
calls left commented out in place. So this project already has every
known fix for this exact class of problem, and is still hitting the
render budget at 6 voices on a filtered, chorused patch, that reads as a
real, current, near-ceiling compute cost for this hardware rather than a
remaining bug to find. `AMY_USE_FIXEDPOINT` is also already unconditional
in this AMY version ("Always use fixed point"), not a lever to flip.

Only remaining cheap, unverified lever: confirm the Arduino IDE's CPU
Frequency board menu is actually set to this chip's maximum (240MHz for
S3), rather than a lower default. Not yet confirmed. Otherwise, the
practical path from here is managing the cost (the `Voices` per channel
setting added two passes ago, keeping heavy filtered/chorused patches at
lower polyphony) rather than continuing to look for another bug.

**Update 2026-09-24 (sixteenth pass): CPU frequency confirmed already
maxed, so on to patch names, a standard OK button, and settings
persistence, with per channel effects and an SD folder-as-voice picker
queued next rather than attempted in the same pass.** Four things landed:

- **Patch names.** AMY exposes no runtime name API (checked `docs/api.md`
  and the whole source tree, not guessed), the "N: Name" the patch table
  carries are C comments, compiled out entirely. Wrote a one-time
  extraction script (parses `patches.h`'s own comments) that generated
  `loopanini/src/patch_names.h`, 391 entries, 266 named, regenerate it the
  same way if `AMY_Synthesizer`'s `patches.h` ever changes. The AMY
  screen's Patch row now opens a dedicated named picker (`drawPatchPicker`/
  `pressPatchPicker` in `ui.cpp`) instead of the punch in editor, opens
  scrolled to the current patch, current patch highlighted. `drawList`'s
  usual page dot strip does not scale to this many pages (391 patches / 5
  a page = 79, `drawList`'s `130/pages` segment height rounds to nothing
  past ~65 pages, a real latent bug in that shared widget, left alone
  since nothing else currently has that many rows, but worth knowing
  about before reusing `drawList` for anything else this large), the
  picker shows "page/pages" as text instead. The AMY summary grid's 2x2
  cells show the patch name now too, not just its number.
- **Big OK button.** Standard on every number pad screen now, filling the
  right column's blank space below Up/Down (and Tap, when present),
  alongside the small OK tile already in the digit grid, same action.
- **Settings persistence.** `Preferences` (NVS), one fixed size
  `PersistedSettings` struct written/read as raw bytes rather than dozens
  of individual keys, magic + version guard against a future field
  addition silently misreading old data instead of corrupting it quietly.
  Debounced rather than hooked into every mute/drag/edit call site:
  `tick()` diffs a fresh snapshot against the last saved one every 3s and
  only writes when something changed, so a fader drag doesn't hammer
  flash and no mutation site can be missed by forgetting to mark it dirty.
  Covers mixer levels/mutes/solos/rec-enable, limiter thresholds, every
  Config value, all 4 AMY channels' chan/patch/vol/voices, and BPM/
  measures. Loading forces `amySeen`/`amyChanSeen` to an impossible
  sentinel so `applyAmy()`'s first pass after boot always pushes every
  loaded value to AMY, even one that happens to match a fresh boot's own
  default, rather than silently no-op'ing because the seen/current pair
  already matched.

Queued next, not attempted this pass, each is its own real UI surface:
per channel effects (echo/chorus/reverb/dist) on/off plus their
parameters, an SD folder picker to use as a channel's voice source
instead of a baked in patch, and written setup instructions for that
folder convention. On the "turn off chorus for headroom" idea
specifically: worth building regardless since it was asked for directly,
but set the expectation honestly, the fifteenth pass's actual scaling
cost was oscillator/filter render (`freq_of_logfreq`, `amp_combine_
controls`, `filter_process`, per audible oscillator), not the bus level
chorus/echo/reverb effects, so this is unlikely to be the dominant lever
for the 6 voice wall, more likely a smaller, separate, still real saving
(chorus specifically runs a per-sample delay line per bus). Not yet
confirmed on hardware.

**Update 2026-09-24 (seventeenth pass): four direct UI corrections to the
sixteenth pass's work.** All in `ui.cpp`:
- The small "OK" tile in the number pad's digit grid is gone, the big OK
  added last pass was meant to replace it, not sit alongside it. Grid is
  11 keys now (1-9, C, 0), last grid slot just blank.
- MIDI Chan's row opens a 4x4 grid of 16 buttons (`drawChanPicker`/
  `pressChanPicker`) instead of the punch in editor, same treatment
  Patch got two passes ago, picking a channel is a single tap.
- The AMY summary screen's patch names were being cut at a guessed
  15-character count. Replaced with `truncateToWidth()`, which measures
  the real pixel width via M5GFX's `textWidth()` and drops characters
  until it actually fits the box, a fixed character count is wrong in
  both directions depending on which letters a given name has (proportional
  font) and was never checked against the real font metrics to begin with.
- Bottom nav (the touch strip below the screen) used to do nothing at all
  from inside any box with its own red X (the numeric editor, an AMY
  channel's view, the patch/channel pickers), the gesture was silently
  swallowed. It now closes whichever of those is open first, same as
  tapping its own X, then navigates, so bottom nav always means "leave to
  the next/prev main screen" regardless of what is open.

Not yet confirmed on hardware.

Main out's Solo spot (soloing the final mix has no meaning) is now a hollow
toggle button showing the 3 letter label of the column it targets (LOP,
INT, ALL or EXT), that cycles and mirrors
Config's Stutter Track setting, so which input the stutter grid acts on can
be flipped from the Mixer screen without a trip to Config. Round mixer button
text nudged 1px down and right (Mute, Solo, Limiter, the Pump button, this new
toggle), the loop-record buttons (icon only, no text) are unchanged. The 4
fader handles moved 1px left. The 3 vertical divider lines between columns are
gone.

**Update 2026-09-25 (eighteenth pass): power off actually powers off, one
more label fix, a real quiet period save debounce, and patch picking
redesigned into categories. Plus a large backlog captured in full so none
of it gets lost, most of it not attempted yet.**

Landed:
- **Power off.** Holding CoreS3's power button looked like it turned the
  board off (screen went dark) while it kept running and draining the
  battery, because nothing in this sketch ever called `M5.Power.
  powerOff()`. CoreS3 only exposes `BtnPWR` (M5Unified's own button
  table), and unlike some M5 devices its long press is not a hardware
  only cutoff, the firmware has to notice the hold and actually act.
  `loopanini.ino`'s `loop()` now calls `M5.Power.powerOff()` on `M5.BtnPWR.
  wasHold()`. Not yet confirmed on hardware.
- **Int/Ext Max Gain's Config label** was overflowing its row, cut off
  right after "Gain". Shortened to "Int Max Gain"/"Ext Max Gain" (dropped
  " Db", which was the part getting clipped).
- **Settings persistence is now a true quiet period debounce**, not
  "write every N seconds while dirty": the seventeenth pass's version
  would actually write mid gesture during a long continuous fader drag,
  since it re-checked and saved every 3s regardless of whether the value
  was still moving. `ui.cpp`'s `maybeSaveSettings()` now tracks a
  `pendingSaved` snapshot and only commits to flash once that snapshot
  has been stable for `kSettingsQuietMs` (5000ms), matching the
  standard requested for the effects parameters below, applied generally
  since the underlying concern (an NVS write catching the flash bus mid
  live tweak) isn't specific to effects.
- **Patch picking is a category menu now**: Juno-6, DX-7, Drum Kit,
  Custom, SD Card, a right pointing triangle marks whichever category the
  current patch is actually in. Juno-6 (patches 0-127) and DX-7 (128-255)
  are contiguous ranges; Drum Kit is a curated list (`kDrumKitPatches`:
  258, 384-390), AMY's patch table names real standalone kits "MIDI
  drums ..." / "drum kit N ...", a few ordinary Juno/DX7 patches also
  have "drum" in their own name (Steel Drums, LOG DRUM) without being a
  kit, so this was hand picked from `patch_names.h` rather than pattern
  matched. Custom and SD Card show in the menu, greyed, not wired to
  anything yet, that's the "Custom preset intake" and "SD folder as
  voice" backlog below. Drilling into Juno-6/DX-7/Drum Kit reuses the
  seventeenth pass's paginated list, now with the same page arrows and
  segmented index strip Config uses (dropped last pass only because it
  breaks past ~65 pages, a single category tops out at 26, so it's back
  and safe). Upper left X goes back one screen (out of a category to the
  menu, out of the menu to the AMY channel's param list), not straight to
  fully closed.

**Backlog, captured in full, not attempted this pass:**

**1. Custom patch intake + SD folder as voice source.** The "Custom"
category should hold patches sent from the AMYboard/Tulip web editor over
MIDI (the user sends from that editor "on ch1"), captured and written as
a file in an SD `Custom/` folder, with a placeholder filename the user
can see and rename from a computer (SD card pulled or mounted), not
hidden. The "SD Card" category should browse folders on the SD card and
load one as a channel's voice, this needs an actual folder browser UI,
nothing like it exists yet (`sample_bank.h`'s kit loading only handles
one hardcoded dir, `LOOPANINI_SD_KIT_DIR`, for channel 10 drums, not a
picker). To keep the SD Card category's folder list from also showing
`Custom/` (patch files, not sample kits, would look like a bogus "folder"
entry there), either exclude that one name specifically, or root the SD
Card browser at a subfolder (the user's own suggestion: something like
`Samples/`) so `Custom/` sits outside it naturally, simpler, prefer this
if it doesn't complicate the eventual `LOOPANINI_SD_KIT_DIR`-style config.
**Needs research before writing any of this**: does AMY (or the Tulip/
AMYboard ecosystem it comes from) already have a standard on-disk patch
file format / a documented way the web editor exports what it sends over
MIDI, so a receiving Custom file is compatible with that ecosystem rather
than inventing a new ad hoc one. Look this up (their docs, their web
editor's own source/export code, their sysex or MIDI patch-dump
convention if any) before designing the file format, adhere to it as far
as practical per the user's own instruction, don't guess a format.

**2. Per channel effects menu.** Each of the 4 AMY channels needs an
Effects entry (from the AMY edit screen) with a submenu per effect:
EQ, lowpass filter with resonance, chorus, reverb, echo, distortion,
saturation (Ableton style), matching what AMY's own web editor exposes
as inspiration. Adjusting one **overrides whatever that channel's loaded
patch itself already sets for that effect** (patches encode their own
chorus/eq/etc in their patch string), a live manual layer on top, not a
replacement of the patch system. Persisted, using the same quiet period
debounce landed this pass. **Each effect needs an easy on/off**, framed
explicitly around managing CPU headroom for polyphony (echoing the
fifteenth pass's finding that chorus/echo/reverb are bus level, not the
per-oscillator render cost that actually caused the 6-voice wall, so
expect these toggles to matter less for headroom than that framing
hopes, still worth having both for the CPU angle and because they're
directly useful controls regardless). Known AMY API surface, from
`docs/api.md`, to build these against:
  - Chorus: `chorus_level, chorus_max_delay, chorus_lfo_freq, chorus_depth`
    (`amy.send(bus=N, chorus=...)`).
  - Reverb: `reverb_level, reverb_liveness, reverb_damping, reverb_xover_hz`.
  - Echo: `echo_level, echo_delay_ms, echo_max_delay_ms, echo_feedback,
    echo_filter_coef` (-1 HPF, 0 flat, +1 LPF).
  - Distortion: `dist_clip`/`dist_fold` (on/off, stack in clip/fold/crush
    order), `dist_drive` (pre-gain), `dist_mix` (wet/dry). At bus scope
    (no `osc` in the event) this addresses the bus's own distortion stage.
  - EQ: exists (`eq` command, 3 band, seen in the Juno patch string
    format itself, `x7,-3,-3` style), needs its exact parameter names
    confirmed from `docs/api.md` before building the UI, not assumed from
    memory the way the four above are (those were read directly this
    session).
  - Lowpass + resonance and saturation: **not confirmed as bus level AMY
    features**, AMY's filter (`filter_process()`, the 6-voice wall's own
    hot path) is per oscillator, set via a patch's own filter fields, not
    obviously a bus effect the way chorus/reverb/echo are. If there is no
    bus level equivalent, "implement simply" per the user's own
    instruction: a basic one/two-pole lowpass with a resonance parameter
    and a simple waveshaper for saturation, run in this project's own
    mixer code (`ui::processBlock()`) the same layer the limiter already
    lives in, one instance per channel, gated by that channel's on/off
    same as the AMY-native effects. Needs each channel actually assigned
    to its own AMY bus first (`synth=N, bus=N`-style, or the drum
    oscillators' own `bus` field for channel 10) for the AMY-native
    effects (chorus/reverb/echo/dist/eq) to be independent per channel at
    all, right now every channel's oscillators are on AMY's default bus
    0 together, confirm `AMY_DEFAULT_NUM_BUSES` (4) or raise `max_buses`
    covers 4 independent channels cleanly before assuming it does.

**3. A new combo effect, not from AMY, built by this project**: a
"musical beat matched tremolo, vibrato, and univibe-style chorus combo"
with three selectable modes (tremolo = amplitude modulation, vibrato =
pitch modulation, univibe-style = phaser/chorus character), all locked to
BPM (internal clock or incoming MIDI clock, this project already has a
BPM concept for the looper to sync to). This is genuinely new DSP this
project has to design, not an AMY parameter to expose, scope it as its
own real effect (rate as a beat division like the Stutter grid's, depth,
mode select) when it's taken up, not squeezed in as an afterthought
alongside the effects menu above. Not to be confused with item 5's Chop
tab below, Chop is a hard on/off square wave gate tied to the beat
division grid, this combo effect is a continuous, smoother modulation,
different DSP, different home (the per channel effects menu, not a
beat division tab).

**4. Mono Retrig for AMY mono voices.** When an AMY channel's voice count
(`synth_engine::setVoices`) is 1, holding two notes and releasing the
newer one should re-sound the older one instead of going silent, a trill
becomes possible, standard mono synth last-note-priority behavior.
Researched against arpnmidi's own equivalent (`/Users/woz/Projects/
arpnmidi`, UI name "Mono Retrig," `drawMonoRetrigScreen`, main .ino around
line 10743): it is one combined mode there, not a separate legato/retrig
split, every note change is a full Off then On, there is no pitch-only
glide variant. Its state is 4 parallel 128 entry tables (held flag,
velocity, source, a monotonic press order stamp) plus one "currently
sounding" record. On note-on: mark held, stamp order, if this note is
already the sounding one just refresh velocity, otherwise Off the
currently sounding note and On the new one. On note-off: clear that
note's held flag, if it was the one actually sounding, scan the held
table for the newest remaining stamp and re-sound that one (the trill),
or Off if nothing is left held. **Not yet researched on our side**: AMY's
own C API for sending a note on/off to a specific already-known
oscillator directly (something like its event struct with an explicit
osc target), since this needs to intercept note on/off ahead of AMY's own
`convert_midi_bytes_to_messages()`/`amy_process_single_midi_byte()`
byte level parsers for any channel in mono mode, across all 3 MIDI
transports (midi_io.cpp, midi_din.cpp, midi_host.cpp), rather than pass
raw bytes straight through like today. Looked bite sized at first glance,
turned out to need this API research first, correctly not guessed at or
rushed this pass.

**5. Beat Division screen, replaces the Stutter screen.** Same shared
grid of divisions as today's Stutter screen, but on 4 tabs (Stutter,
Chop, Arp, Drum Roll) instead of one screen, squished left slightly to
fit a right side scrollbar that switches between tabs (an unusual
scrollbar-as-tab-switcher, not a page indicator). Top middle shows the
current tab's name, top right a gear icon opens that tab's own settings
screen, a plain parameter list like Config's (`drawList`), not a new
layout to invent. Chop ("slicer"): a square wave tremolo, on/off gating
at the chosen division, uses the same input setting as Stutter (the
Mixer's LOP-column hollow circle, `cfgStutTrack`, moved there this pass,
see below) rather than a second, separate input toggle. Arp and Drum
Roll are the two tabs that need real new engines, not just a new grid
skin, see items 6 and 7.

**6. Arp settings and Thru channel.** Full settings list, sourced from
arpnmidi's own Arp submenu (main .ino lines 10579-10580, matching its own
FIRMWARE_3_PLAN.md lines 155-156): Mode, Division, Arp Velocity, Arp
Length, Octave Range, Retrig, Order, Length, Learn Custom Arp, Clear
Custom Arp. Retrig here is arpnmidi's own separate setting (Clock Sync
vs Key Press, whether a fresh key press plants a new phrase origin), not
to be confused with item 4's Mono Retrig, same word, different feature,
keep them distinct when this is built. Custom arp capture: starts on the
first note after Learn is armed, ends at a musical boundary or an
explicit stop, stores up to 32 events (start, gate, velocity, pitch
offset) measured from the take's lowest note, remapped live to whatever
is lowest held at playback. Thru: a second, separate channel setting
(arpnmidi keeps it as its own peer screen, we can nest it inside Arp
settings as a second Ch field instead, the concept ports either way) that
forwards a raw, non-arpeggiated copy of the arp input channel's notes to
its own output channel, off when set to channel 0. Both Arp and Drum Roll
get a plain Ch setting on their main settings screen, Drum Roll's
defaults to channel 10.

**7. Div Map: note/CC learn for Drum Roll, Chop, and Stutter divisions.**
Called "DIV NOTES" in arpnmidi, not "div map", worth keeping in mind when
grepping its source later. Its settings screen has one slot per division,
tap a slot to select it, the very next note or CC number and channel that
arrives while that slot is selected gets bound to it, no separate
"listening" flag, capture is simply gated on being on that screen with
that slot selected. Live, independent of whatever screen is currently
showing: every incoming note or CC is checked against all bound slots, a
match sets that slot's held flag and a press order stamp, whichever held
slot has the newest stamp wins and is read by the mode's scheduler ahead
of its stored division, snapping back the instant the mapped note/CC
actually releases (edge triggered on note-off or CC < 64, not a timer).
**Unconfirmed, check before building**: arpnmidi's own source left it
unclear this pass whether holding a mapped slot also force-enables Drum
Roll's master on/off, or only overrides the division while it's already
on, the user wants the force-enable behavior ("momentarily activate drum
roll if not activated"), confirm arpnmidi actually does this before
assuming the porting is 1:1.

**8. A MIDI data looper, separate from the audio looper.** Same tied
sync behavior as the audio looper's own tracks: each track remembers its
phase relative to the shared transport, so stopping and restarting keeps
it aligned to where it was originally recorded rather than zeroing back
to the start, unless it's a genuinely fresh recording. Sourced from
arpnmidi's `four_track_looper.cpp`'s `captureTrackPhase()` (line
228-241): it stores `startOffsetUs = (cycleStartUs - transportStartUs) %
lengthUs`, how far into its own loop length the track was when the
shared transport last started. Also from that same source: a per track
tappable Quant button, a raw microsecond quantize step independent of
BPM (`recordQuantizeUs_`, four_track_looper.h line 62/141), while BPM
stays one shared global transport tempo, exactly like the audio looper
already has it. The audio looper's own BPM display slot is reused to show
Quant instead when this screen is showing the MIDI looper, not a new
slot, per channel screen real estate is already tight.

**9. Input/Output source routing (Config screen).** Two new global
settings: Input Int/Ext (CoreS3's own mic vs ModuleAudio's input jacks)
and Output Int/Ext (CoreS3's own speaker vs ModuleAudio's headphone jack).
Confirmed from M5Unified source (`Speaker_Class.hpp`/`Mic_Class.hpp`):
`M5.Mic.record(int16_t*, len, sample_rate, stereo)` and `M5.Speaker.
playRaw(int16_t*, len, sample_rate, stereo, repeat, channel,
stop_current_sound)` are shaped comparably to `audio_io.cpp`'s existing
`device.record()`/`device.play()` calls into ModuleAudio, both are
task-based blocking-per-block APIs, encouraging, this may be closer to a
routing branch inside `readBlock()`/`writeBlock()` than a redesign.
CoreS3's internal mic/speaker very likely sit on a separate I2S
peripheral instance from ModuleAudio's (the ESP32-S3 has two), so both
initializing at once without conflict is plausible, not yet confirmed,
and switching behavior (can both run simultaneously with only one
actually read/written per block, or does switching need a teardown/
reinit of whichever side is inactive) is not yet designed.

**Update 2026-09-25 (nineteenth pass): a number pad shortcut for patch
picking, swipe to scroll on Config and the patch categories, and a
standing 1px nudge on every back X.** All in `ui.cpp`:
- A yellow button, 3x3 dot grid icon, sits upper right on the patch
  category menu (mirrors the upper left X). Opens the same numeric editor
  every other setting uses (`openEditor("Patch", &amy[amyEdit].patch, 0,
  kPatchNameCount - 1, false)`), so a specific patch number can be typed
  directly instead of drilling through categories. Needed a forward
  declaration of `openEditor` (defined much further down with the rest of
  the editor's own logic) since this is the first thing to call it from
  earlier in the file.
- **Swipe to scroll**, on Config and a patch category's list (the only
  two screens with real pages, the category menu and the 4/16 button
  grids have none). This needed onPress itself restructured, not just an
  addition: S_AMY and S_CONFIG's presses used to act immediately, which
  would have meant a swipe starting on top of a row also selected that
  row on the way through. Both are now deferred to release, where
  `wasTap()` (moved less than 12px) decides between a normal row tap
  (`pressAmy`/`pressList`, same as before, now called with the release's
  base position) and `trySwipePage()` (moved more than 40px vertically,
  changes the page instead). Every other screen (Mixer, Looper) still
  acts on press, unaffected, dragging a fader or holding a stutter pad
  needs that immediacy and has nothing to page.
- Every upper left red X (Config, the AMY channel view, both pickers) now
  goes through one shared `backButton()` instead of a separate `button()`
  call each place, character nudged 1px right of center, standard
  everywhere it appears, same idea as `circleButton()`'s own existing
  +1,+1 nudge.

Not yet confirmed on hardware.

**Update 2026-09-25 (twentieth pass): SD Card and Custom actually do
something now, researched against AMY's own documented conventions
first, not invented.** Research before writing anything, per the user's
own instruction not to reinvent a format:
- **AMY's "patch file format" is just its own wire protocol text.**
  `patches.h`'s baked in patches (`"v1w4a1,,0,1Zv0w20c2L1G4Z..."`) are the
  format, there is no separate binary/JSON patch file convention to be
  compatible with. Confirmed in `docs/midi.md`'s SYSEX section: AMY
  receives wire messages over SYSEX directly, no encoding, prefaced with
  its own manufacturer ID `00 03 45` inside standard `F0 ... F7` framing.
  This is almost certainly how "the AMYboard editor sending on ch1"
  actually delivers a patch.
- **User patch numbering is already a documented AMY convention**, not
  something to invent: `docs/synth.md`'s "User patches" section fixes it
  at 1024-1055 (32 slots, `amy_default_config`'s `max_memory_patches`),
  built by repeating `amy.send(patch=SLOT, osc=N, ...)` for each
  oscillator-describing command. `LOOPANINI_USER_PATCH_BASE`/`_COUNT`
  added to `config.h` matching this exactly. Also cross-confirms this
  project's own hand curated Drum Kit category from the nineteenth pass:
  `docs/api.md`'s `patch_number` row spells out "256 piano, 258 legacy GM
  drums, 384-390 Gamma9001 GM drum kits" as the official ranges, exactly
  `kDrumKitPatches`.

Built:
- **SD Card** browses `LOOPANINI_SD_VOICES_DIR` (`/Samples`, a subfolder
  specifically so `/Custom` doesn't also show up as a bogus kit, the
  user's own suggested fix), `sample_bank::listFolders()` (new), tapping
  one calls the existing `synth_engine::loadDrumKit()` on it. Only
  enabled when editing channel 10: that is the only channel `synth_engine
  ::routeDrumNote()` actually intercepts samples for, enabling it
  elsewhere would look like it worked and do nothing. Generalizing kit
  playback to channels 1-3 is the real next step for this category, not
  done here.
- **Custom**: capture, save, and browse all work, applying a loaded one
  is the piece still marked best effort. `midi_io.cpp`'s USB device MIDI
  path now accumulates SYSEX across USB-MIDI's own multi packet framing
  (Code Index Number 4-7), and on a complete message carrying AMY's `00
  03 45` prefix, saves the wire text as `/Custom/patchNNN.txt`
  (`sample_bank::saveCustomPatch()`, numbered placeholder, not hidden,
  the user can rename it from a computer). The Custom category lists
  those files (`sample_bank::listCustomPatches()`) and, on tap, reads one
  back (`loadCustomPatch()`) and replays it through `synth_engine::
  applyCustomWire()`, which is a direct, one line call to AMY's own
  `amy_send_wire_from_sysex()`, the same entry point a live incoming
  SYSEX patch goes through. **What's unverified**: whether replaying the
  raw captured text this way is sufficient on its own, or whether it
  needs to be wrapped as a proper registered user patch first (`patch=
  SLOT` on each command, per the User patches section above) to reliably
  land on the right synth/channel rather than whatever `v0`/`v1`/etc in
  the text happens to resolve against live. Only USB device MIDI captures
  SYSEX right now, not USB host or DIN, matching that a web editor most
  plausibly connects as this board's USB device (computer side), not
  through Module USB host or DIN; extending capture to those two is
  possible later if needed, DIN especially is a bigger lift, no flow
  control at 31250 baud for a large dump.
- Each of the AMY screen's 4 boxes shows its channel's 3 digit patch
  number, upper right, measured/positioned the same right-align-by-
  measuring approach `truncateToWidth` already uses, `text()` itself only
  centers or left-aligns. Reads 1024+ automatically once a Custom patch
  actually lands in the user patch range, no special casing needed for
  that, it is already just `amy[i].patch`.

Not yet confirmed on hardware, especially the Custom apply step flagged
above, that is the one piece of this pass built on inference about AMY's
event semantics rather than something read directly and confidently from
source, say so plainly if it does not work as hoped on the first try.

**Update 2026-09-25 (twenty first pass): the overload failsafe wired up
in the ninth pass was a severe regression, found with certainty and
fixed.** Hardware report: AMY made sound for "a few notes" right after
boot, then MIDI stopped reaching it entirely, on USB host and USB device
both, while the UI stayed fully responsive. Traced to source rather than
guessed, since this exact combination (both transports dead, UI fine)
pointed at shared AMY-side state, not a transport bug: `amy_overload_
failsafe()` (amy.c) calls `amy_reset_oscs()`, which runs `instruments_
reset()` and `pcm_unload_all_presets()`. That does far more than its own
"silence and reset" framing suggests, in api.c's own comment right above
it, it permanently wipes every channel's synth assignment (`default_
synths`, set up once at `amy_start()`, nothing in this project recreates
it after) and every SD loaded sample, the instant it fires once. The
"few notes" was its own descending "doot doot doot doot" arpeggio playing
as it fired, not real MIDI activity, and after that there were no synths
left on any channel for MIDI to reach, matching the report exactly.

Fixed with a one line, source confirmed change: `synth_engine::begin()`
now sets `amy_config.overload_threshold = 0`. Confirmed from `amy_
overload_check()`'s own code this disables only the destructive branch
(the threshold gate is checked after the smoothed load value already
updated), `amy_get_render_load()` reads that same smoothed value
directly and is confirmed independent of the threshold, so the render
load line this project's debug output depends on is unaffected, only the
`instruments_reset()`/`pcm_unload_all_presets()` action is disabled. The
hook (`onOverload`, eighth pass) is left registered but now unreachable,
kept as a documented, ready-made hand-hold for a safer failsafe design
later (mute output briefly, say, rather than wipe every instrument) if
one is ever wanted, not deleted.

Confirmed on hardware: not yet, please retest MIDI on both transports.

**Update 2026-09-25 (twenty second pass): a version string on the boot
splash, and the upload log itself turned out to be worth reading.** The
retest's own upload log showed `loopanini.ino.bin` (the actual firmware,
not just the bootloader/partition table) hit esptool's "No changed
sectors found, verifying if data is in flash" path, meaning that specific
upload may have written nothing new to the chip at all, esptool's own
speed optimization for "this content already matches flash." Ambiguous on
its own (could mean a stale build, or could just mean an earlier upload
already carried the fix and this one had nothing left to change), but not
something to guess past when the user directly asked for a way to make
this visible. `config.h`'s new `LOOPANINI_VERSION` ("v0.1.1") now prints
on the boot splash, right before "starting...", bump it every pass from
here whenever a change goes out for testing. Also cleared this project's
specific Arduino build cache directory (`~/Library/Caches/arduino/
sketches/...`, the exact one the log named) to remove any doubt for the
retest, that is pure compiler output, safe and normal to clear, Arduino
regenerates it fully on the next compile.

**Update 2026-09-25 (twenty third pass): the real cause of "AMY dies at
boot", a debug log filter, and one more meter SPI reduction.** The actual
bug, found on hardware, not in any of this project's own code changes: a
channel's MIDI Chan had been changed (most likely while trying the
nineteenth pass's new channel picker) and, since the twentieth pass added
settings persistence, that saved and reloaded on every boot from then on.
Notes arrived and parsed correctly on both transports the whole time
(confirmed from real hardware logs, ruling out a transport bug directly
rather than assuming one), nothing was listening on the channel they
were actually sent on. The overload failsafe fix (twenty first pass) was
real and correct but was never the (sole) cause here, worth remembering
that two things can be broken at once. Fixed on hardware by correcting
the channel, no code change needed, the lesson for next time either of
"AMY makes no sound" recurs: check MIDI Chan on the AMY screen first,
it is right there, large, on every channel's box already.

Also: `midi_host.cpp`/`midi_io.cpp`/`midi_din.cpp` no longer print real
time bytes (0xF8-0xFF: clock, active sensing, ...) to the debug log, some
controllers send clock continuously and it was flooding the log
unreadable, this only affects what gets printed, not processing or
timing. And the Mixer meter SPI throttle (twelfth pass) widened further:
hardware testing found a little pop/crackle still came back specifically
when pushing the limiter hard on INT and Main together, most likely
because a heavily limited signal's displayed level genuinely swings more
block to block, crossing the old 0.01 "did it move" threshold almost
every 100ms tick. Now 160ms / 0.025, trading a little meter smoothness
for less SPI traffic specifically in that scenario. `LOOPANINI_VERSION`
bumped to v0.1.2 for this pass (v0.1.1 shipped a code change, the debug
filter, without bumping the version, a miss worth naming so it does not
happen again).

Not yet confirmed on hardware whether the meter throttle change actually
clears the last bit of pop/crackle under heavy limiting.

**Update 2026-09-25 (twenty fourth pass): looked at making VU draw
genuinely unable to interfere with audio, beyond the throttle alone.**
Traced the real mechanism instead of guessing at FreeRTOS priority tuning.
Confirmed from source: Arduino's `loop()` task (where all UI drawing,
including the Mixer meter, runs) is created at priority 1
(`cores/esp32/main.cpp`), well below the audio task's priority 5
(`LOOPANINI_AUDIO_TASK_CORE`/`_PRIORITY`, config.h), and the audio task is
explicitly pinned to core 0. `m5stack_cores3`'s boards.txt does not
hardcode a default core for `loop()` itself, though it does expose a
LoopCore menu choice, so which physical core UI drawing actually lands on
was not something to assume, a one line `xPortGetCoreID()` print was added
right after setup()'s existing reset reason line instead (see
loopanini.ino) so this is confirmed from the boot log every time, not
assumed once and forgotten.

Also confirmed, tracing M5GFX's actual SPI bus code
(`Bus_SPI.cpp`/`Panel_LCD.cpp`): every `pushSprite()` already goes over
hardware DMA, and every `pushSprite()` also busy spins the calling core
(`while (*spi_cmd_reg & SPI_USR);`, no yield) until that same transfer
finishes, inside the same call, with no exposed way to kick a transfer off
and come back for it later without holding the shared `spi_lock` mutex
(also used by Module USB) open across ticks, which would trade this
problem for a worse one. So "switch it to DMA" was not the real lever, it
already is, and there is no clean async option here without a much bigger,
riskier restructure. Dropped that specific idea rather than ship something
that only sounds like a fix.

What actually shipped: `drawTrack()`'s meter push used to redraw and push
the full `kTrackW` (25px) wide column sprite for every qualifying change,
including the by far most common one, the level bar moving with no fader
or mute change. Split into `drawTrack()` (unchanged, full column, used
whenever the fader or mute dot actually changed) and a new
`drawMeterOnly()` (a separate, narrow 8px wide sprite covering just the
bar's own width, reused across all 4 columns the same way `track` already
is). `tick()` now checks level movement and fader or mute movement
separately and picks whichever draw the change actually needs. The fader
handle's circle footprint still reaches into that narrow strip whenever
it's sitting anywhere near it (it spans the track's full width), so
`drawMeterOnly()` redraws it too, at its unchanged position, same as
`drawTrack()` does, otherwise the meter only path would silently erase
part of it, caught before shipping by tracing the actual pixel geometry
rather than assuming an 8px wide push was automatically safe. Net effect:
the common per tick case now pushes roughly a third the bytes over SPI it
used to, on top of the existing 160ms/0.025 throttle (twenty third pass),
so whatever else wants the bus at that moment finishes sooner.

Not yet confirmed on hardware whether this narrows the heavy limiting
pop/crackle further. `LOOPANINI_VERSION` bumped to v0.1.3.

**Update 2026-09-25 (twenty fifth pass): USB hub support, a Mixer
rearrangement, and a big backlog addition researched against arpnmidi.**
Landed:
- **USB hub support was missing, now added.** The vendored USB Host
  Shield 2.0 library (`src/USB_Host_Shield_Library_2.0/usbhub.h`/`.cpp`)
  already has a hub driver, but `midi_host.cpp` never instantiated one,
  only `USB` and `USBH_MIDI`. `USB::Task()` walks every registered device
  class driver each call, so a hub plugged into Module USB would enumerate
  itself (or fail) but nothing would ever poll its downstream ports,
  whatever was plugged into the hub would never be seen. Fixed with a
  3 line addition (`USBHub *hub = nullptr;` and `hub = new USBHub(usb);`
  in `begin()`, same pattern already used for `midi`), `USB_NUMDEVICES` is
  16, plenty of headroom, no other change needed. Not yet confirmed on
  hardware, needs an actual hub plugged in to test.
- **Mixer rearrangement.** The stutter/chop input selector (the hollow
  ring cycling through `cfgStutTrack`) moved from the Main column's row 1
  to the LOP column's row 3, previously the only blank spot in the grid,
  the same row the other two loop record-arm buttons sit in. The spot it
  vacated, Main's row 1, is now a hollow red ring that will fill solid
  red while a master mix recording to SD is in progress (`sdRecording` in
  ui.cpp), not wired to a real recorder yet, that's item 1's SD work,
  still not attempted, so it stays hollow today, honestly, not faked.

Also confirmed already fully captured, no change needed: the open
research question on whether AMY/Tulip has a standard on-disk patch file
format (item 1 above) was already written down accurately, re-read it and
left it as is.

The rest of this pass was research, not code, folded directly into items
4 through 9 of the backlog above (Mono Retrig, the Beat Division screen,
Arp settings and Thru, Div Map/DIV NOTES, the MIDI looper, and Input/
Output source routing), each one sourced against arpnmidi's actual code
this time rather than working from the user's description alone, per
their own standing instruction to look real things up before designing
around them. `LOOPANINI_VERSION` bumped to v0.1.4.

**Update 2026-09-25 (twenty sixth pass): Mixer and Stutter buttons no
longer redraw the whole screen.** Both screens set a single blanket
`dirty` flag on every button tap, which redraws everything (Mixer:
`fillScreen` plus all 4 fader sprites, all 16 circle buttons, all 4
labels; Stutter: `fillScreen` plus all 12 cells) for one button's color
changing, exactly the same class of unnecessary SPI traffic the meter
throttle (twelfth and twenty third passes) and the meter-only narrow push
(twenty fourth pass) already went after, just on the press path instead
of the periodic tick. User caught it directly: tapping a Lim button mid
performance pops, and the Stutter grid, meant for rapid expressive taps
and drags, was the worst offender.

`drawMixer()`'s per row drawing split into standalone functions
(`drawMuteBtn`, `drawRow1Btn`, `drawLimiterBtn`, `drawRow3Btn`,
`drawTrackLabel`), `drawMixer()` itself now just calls all of them per
column, so there is exactly one place each button's drawing logic lives.
`pressMixer()` calls only the one (or, for Solo, since `anySolo` dims
every other column's label too, one button plus all 4 labels) needed for
whatever it just changed, wrapped in its own `spi_lock::Guard`, no more
`dirty = true` anywhere in it. Same split for Stutter: `drawStutterCell(i)`
factored out, `whileHeld()`'s drag-across-cells handling and `update()`'s
release handler each redraw only the cell(s) that actually changed state
(old cell back to normal, new cell to orange), not the full 12. Screen
entry/navigation still goes through the normal `dirty` to `redraw()` full
paint, correctly, that genuinely needs everything drawn once.

Not yet confirmed on hardware. Also noted, not yet investigated: user
reports pops still occasionally happen within 6 voices with simple, light
effects, slightly worse specifically over USB host MIDI, separate from
this pass's fix, still open. `LOOPANINI_VERSION` bumped to v0.1.5.

Two small follow up fixes on the same screen, same pass: the stutter/chop
input selector (LOP column's row 3) cycled its displayed label in
`cfgStutTrack`'s own raw storage order (LOP, INT, ALL, EXT) rather than
the visual column order the user actually sees left to right, tapping now
advances the displayed column (INT, EXT, LOP, ALL, wrapping) and looks up
which stored value shows that column, `cfgStutTrack` itself still stores
the stutter engine's own Looper/Synth/Main/Aux convention unchanged, only
the tap-to-tap cycling order changed, see `kColToEngine` in `pressMixer`.
Also, that button's and PMP's label text were both undersized (1 instead
of the normal 2) to fit their 3 letter labels, bumped to 1.5, in between
rather than all the way to 2, still worth a look on real hardware to
confirm it actually fits now. `LOOPANINI_VERSION` bumped to v0.1.6.

**Update 2026-09-25 (twenty seventh pass): CoreS3's own speaker, wired for
real into Audio Out, an output buffer setting, and a dead placeholder
removed.** User decided against ever using CoreS3's own internal mic
("i dont need the mic... so we dont get feedback ever"), which drops the
whole input side GPIO conflict and the Mic_Class async bridging this was
originally scoped with, real simplification. What shipped instead:

- **Audio Out (Config) is 5 options now and actually wired up**: USB, Ext,
  Ext+USB, Int, Int+USB (Ext not Aux, to match the Mixer screen's own EXT
  column naming for the same physical path; Both renamed to Ext+USB first,
  ambiguous once there were two kinds of "both"; both catches were the
  user's, not caught here first), was 3 options
  (`kAudioOut`) that nothing in the
  codebase ever read, a placeholder exactly like SD Record below. `audio_io.h`'s
  `AnalogOut` (None/Module/Internal) and a bool for whether USB is on are
  both decided once, in `audio_io::begin()`, from a preference persisted
  in its own dedicated NVS key (namespace `loopaudio`, not ui.cpp's
  settings blob), because `begin()` runs before `ui::begin()` and that
  blob's load. `cfgAudioOut` in ui.cpp is a display/edit mirror only, its
  own comment explains why `applySettings` deliberately does not restore
  it, `loadSettings` pulls the real value from `audio_io::outputPref()`
  instead, and `maybeSaveSettings` calls `audio_io::setOutputPref()`
  alongside its existing quiet debounce write, piggybacking on that
  existing timing rather than a second one. Boot time only, confirmed no
  clean way around that this pass: ModuleAudio's I2S is one coupled full
  duplex peripheral (`I2S_MODE_TX | I2S_MODE_RX` in one
  `i2s_driver_install()`, `M5Module_Audio.cpp:406`), and it physically
  shares 3 of its 5 pins with CoreS3's own internal speaker (GPIO0, 13, 14,
  confirmed against both `audio_io.cpp`'s own pin claims and M5Unified's
  CoreS3 board case), so Aux and Int can never both be live. Choosing Int
  or Int+USB silences ModuleAudio's aux input jack too, not just its
  output, same coupled peripheral, no way to leave one side off. USB
  itself is a fully separate subsystem (TinyUSB), no such restriction,
  `loopanini.ino`'s `usb_audio_out::writeBlock()` call is now gated on
  `audio_io::usbAudioEnabled()` so picking Aux or Int alone actually turns
  it off rather than leaving it silently running regardless, which is
  what it did before this pass.

- **CoreS3's internal speaker path is real, not a stub.** `M5.Speaker.
  playRaw()` queues a request to its own background task and returns
  immediately, confirmed from `Speaker_Class.hpp`'s own doc comments,
  unlike ModuleAudio's `device.play()` which only returns once the data
  is actually copied out. Reusing one buffer the instant `playRaw()`
  returns would race that background task still reading it, so this uses
  two buffers, alternated, each reused only once `setBufferReleaseCallback`
  confirms the task is actually done with that specific pointer, exactly
  the pattern the library's own docs recommend. Not yet confirmed on real
  hardware, this pass was implementation, not a bench test.

- **Out Buffer (Config), 0-4 blocks of deliberate slack between render/mix
  and the actual hardware write.** Reasoned through with the user first:
  today there is zero buffering in that chain, a straight render, mix,
  write sequence every block, so an occasional slow render (heavy
  polyphony, patch 0 at 6 voices, already known to sometimes exceed one
  block's time budget) has nothing to absorb it. `audio_io::writeBlock()`
  now queues incoming blocks into a small ring and only starts actually
  writing to hardware once the configured depth is banked, so a rare slow
  render draws down that reserve instead of directly starving the output,
  at the cost of depth blocks of fixed added latency (0, the default,
  is today's exact direct write behavior). Pure software queue, no
  peripheral involved, so unlike Audio Out this applies live,
  `ui.cpp`'s `tick()` just calls `audio_io::setOutBufferBlocks(cfgOutBuffer)`
  every tick, cheap enough not to need its own change check. This helps an
  occasional slow block, it will not help a sustained overload the whole
  time a patch is playing, that needs less render cost or fewer voices,
  buffering only relocates when a backlog would show up, it cannot create
  more CPU time. Separately, not yet investigated: pops specifically worse
  over USB host MIDI are suspected to be `midi_host::poll()` (runs on the
  audio task, takes the same `spi_lock` the LCD uses, confirmed busy-spin
  wait, no timeout) contending with a big LCD SPI push at the wrong
  moment, a different mechanism this buffer would not address either,
  still open.

- **SD Record (Config) removed.** Confirmed dead, like Audio Out was:
  declared, shown, persisted, never once read by anything that would
  actually record to SD. `PersistedSettings`' `kSettingsVersion` bumped
  1 -> 2 for this pass (SD Record's field dropped, Out Buffer's added),
  which means, expected and correct, not a bug: the very first boot after
  this update ignores the old saved blob entirely (version mismatch) and
  starts every setting fresh at its compiled in default, exactly once.

`LOOPANINI_VERSION` bumped to v0.1.7, then v0.1.8 for the Both -> Ext+USB
rename (Aux -> Ext, requested right after, rides along in the same
v0.1.8, not its own bump).

**Update 2026-09-25 (twenty eighth pass): the Looper screen redraw fix,
a real Audio Out persistence bug found on hardware, an icon flip, and a
USB audio question answered.** User caught the Looper screen still doing
the same whole screen redraw thing the Mixer and Stutter passes had
already fixed.

- **Looper screen.** Split the same way Mixer's row functions were:
  `drawSlotBtn(i)`, `drawBpmMeasBoxes()`, `drawStopBtn()`, `drawPlayBtn()`,
  `drawLooper()` itself just calls all of them once. Slot selection
  (`pressLooper`) now redraws only the old and new slot box instead of
  setting the blanket `dirty` flag. More interesting: 3 separate checks in
  `tick()` (a reject flash, its timeout, and the looper state/overdub/undo
  poll) were ALSO setting that same blanket flag, meaning a background
  loop's state changing while looking at the Mixer or Config screen would
  fully redraw whatever screen was actually showing, not the Looper
  screen the change was even about, real waste on top of the redundant
  kind already fixed. Now tracked as one `transportChanged` bool and only
  acted on, redrawing just `drawStopBtn()`/`drawPlayBtn()`, when the
  Looper screen is actually the one visible and no editor is open over
  it, same guard shape as the Mixer meter tick.

- **Audio Out setting did nothing on hardware, a real bug, not user
  error.** Its actual persistence (`audio_io::setOutputPref`, since
  `audio_io::begin()` needs the value before ui.cpp's own settings blob
  even loads) was piggybacked on `maybeSaveSettings`'s 5 second quiet
  debounce last pass, for simplicity. That debounce exists to stop an NVS
  write landing mid gesture on a continuously moving value like a fader,
  but Audio Out is a single discrete tap, there is no gesture to wait
  out, and the only way this setting ever takes effect is a reboot, the
  natural test is change it, reboot immediately, which loses the change
  if done inside that 5s window, exactly what happened. Fixed with a
  small dedicated `pushAudioOutPref()`, called every `tick()`, pushing the
  instant `cfgAudioOut` actually differs from what was last pushed, no
  debounce. `maybeSaveSettings` still carries `cfgAudioOutP` in the blob
  purely so its memcmp notices a change happened at all, it no longer
  persists it.

- **The looper record-arm circle's icon (I_LOOP, both the INT and EXT
  columns share this one shared `icon()` case) flipped horizontally**,
  per direct request. An arc's horizontal flip is its two angles swapped
  (true whenever they sum to 180 mod 360, confirmed true for this icon's
  300/240 pair), a shape's flip negates each point's x offset from center,
  y unchanged.

- **USB audio device question, answered, not a bug.** `usb_audio_out` is
  real and complete (`USBAudioCard(..., UAC_SPK_NONE, UAC_MIC_STEREO)`,
  `usb_audio_out.h`'s own comment: "output-only (device to host)... no
  USB playback path exists"), and unaffected by this session's Audio Out
  work, `usb_audio_out::begin()` (the enumeration call) was never gated,
  only `writeBlock()` (whether samples actually flow) was. Loopanini
  shows up as a CAPTURE/microphone source on the host, by design, not
  as an output or speaker choice, that's where to look for it.

Looper and Audio Out fixes not yet confirmed on hardware.
`LOOPANINI_VERSION` bumped to v0.1.9.

**Update 2026-09-25 (twenty ninth pass): Audio Out gets its own confirm
and reboot screen, USB naming looked into.** User asked directly: does it
really need a reboot? Confirmed yes, unchanged from the twenty seventh
pass's finding (ModuleAudio's coupled full duplex I2S, 3 shared pins with
CoreS3's internal speaker, no clean teardown API either side), then
proposed the actual fix for the confusing part: a dedicated screen,
select a candidate, Apply reboots immediately, Cancel discards, rather
than the previous pass's tap-cycles-in-place-and-silently-applies-
whenever-you-next-reboot behavior that caused the persistence bug found
last pass in the first place.

Built as `drawAudioOutPicker()`/`pressAudioOutPicker()`, opened by
`pressConfig()` (new, replaces the direct `pressList(kConfig, ...)` call
in `update()`) intercepting a tap on the Audio Out row specifically,
found by pointer identity against `kConfig` (`kConfig[i].value ==
&cfgAudioOut`), not a hardcoded row index, stays correct if `kConfig`'s
order ever changes. 5 selectable rows, a hint line explaining whatever is
currently highlighted (not whatever is currently active) so it updates
live while choosing, CANCEL and APPLY, REBOOT as two large buttons rather
than reusing the small back X every other picker in this codebase uses,
per the user's own request for something that unambiguous given the
consequence. Apply calls `audio_io::setOutputPref()` then `esp_restart()`
directly, no round trip through `cfgAudioOut` or the settings blob at
all. This makes last pass's `pushAudioOutPref()`/`lastPushedAudioOut`
dead code, since nothing changes `cfgAudioOut` mid session anymore,
removed rather than left behind, `cfgAudioOut` is now purely
loadSettings()-populated-at-boot, display only.

USB device naming: asked whether "TinyUSB UAC1" (what `usb_audio_out`
shows up as) could become something custom, e.g. "WOZ.LOL". Traced to
`tinyusb_add_string_descriptor("TinyUSB UAC1")` in `USBAudioCard.cpp`,
confirmed a hardcoded literal, no constructor parameter or setter exists
for it. Important distinction from `USB_Host_Shield_Library_2.0` (this
project's own vendored, patchable copy under `src/`): `USBAudioCard` is
part of the ESP32 Arduino core's own bundled libraries, installed
globally under Arduino15's package folder, outside this project and its
git history entirely. Patching it would mean editing a file this project
doesn't own, invisible to version control, silently reverted by any
future core update, affecting every other sketch on this machine, not
just Loopanini. Not done, disproportionate for a cosmetic string, matches
the user's own "if not, whatever."

Not yet confirmed on hardware. `LOOPANINI_VERSION` bumped to v0.1.10.

**Update 2026-09-26 (thirtieth pass): Measures redesigned, a real icon
bug properly fixed this time, Audio Out moved to the top, and a real
internal speaker regression root caused from hardware reports.**

- **Measures** no longer opens the numeric keypad screen. `kMeasuresSteps
  = {1,2,4,8,16}` (a musically useful doubling sequence, not every
  integer 1-8, `looper.cpp` just multiplies `measures * beats`, any
  positive integer was always fine, the old 1-8 range was a UI choice,
  not an engine one), tapping the MEAS button's top half steps up that
  sequence, the bottom half steps down, both wrapping. `drawBpmMeasBoxes`
  split into `drawBpmBox`/`drawMeasBox` so the tap only redraws the one
  box that changed, same discipline as the rest of this session.

- **The I_LOOP icon flip from two passes ago was actually wrong**, confirmed
  on hardware, "broke completely, nearly missing". Traced properly this
  time: swapping an arc's two angle arguments assumed fillArc always
  draws the same arc regardless of argument order. It does not.
  M5GFX's `fill_arc_helper` computes a `reversed` flag from how start and
  end compare to each other, not just their values, picking a "major" or
  "minor" arc, so swapping silently switched a roughly 300 degree ring
  into a roughly 60 degree sliver. The correct fix needed no change to
  the arc at all: its own two angles (300, 240) sum to 180 mod 360, which
  is exactly the condition for that arc's covered angle set to already be
  its own mirror image across the flip axis, confirmed by expanding both
  angles' actual covered ranges by hand, not just asserting it. Only the
  triangle (the arrowhead, genuinely asymmetric) needed its points
  flipped, and did, correctly, in the original attempt.

- **Audio Out moved to the top of Config**, requested directly, safe
  because `pressConfig` finds it by pointer identity, not a hardcoded
  index.

- **Internal speaker root cause, from real hardware reports**: Int (and
  Int+USB) sounded "glitchy and garbled, low and chopped up, almost ring
  mod, with tiny bits missing", constantly, not intermittently. Traced to
  `Speaker_Class`'s own defaults: `task_priority` 2, well below this
  project's audio task (5), and `task_pinned_core` `~0`, meaning
  M5Unified leaves it to FreeRTOS which core to run its background task
  on, unlike everything else in this project, which pins deliberately.
  Landing on the same core as the audio task would starve it, since the
  higher priority audio task preempts it whenever both want to run,
  meaning the release callback (and the 2 alternating buffers depending
  on it) could easily fall behind the roughly 2.7ms cadence blocks
  actually arrive at, forcing the "drop rather than corrupt" fallback to
  trigger constantly instead of rarely, exactly matching "constantly",
  not "occasionally". Fixed in `beginInternalSpeaker()`: reads
  `M5.Speaker.config()`, sets `task_pinned_core` to whichever core the
  audio task is NOT pinned to, writes it back before `begin()`. Also
  bumped from 2 buffers to 3 for a little extra margin against whatever
  scheduling jitter is left on its own core, cheap insurance once the
  real fix (the pinning) is in.

- **Loud pop on reboot, both Int and Ext, looked up**: a software reset
  (`esp_restart()`) does not wind peripherals down first, whatever the
  codec or internal speaker's DAC/amp was last outputting cuts off mid
  stream, heard as a pop, a well known class of issue on audio hardware
  generally, not specific to this board, standard fix is muting before
  the deliberate reset. New `audio_io::muteBeforeReboot()` (`device.
  setMute(true)` for Module, `M5.Speaker.stop()` for Internal, then a
  50ms settle delay), called from the Audio Out picker's Apply button
  right before `esp_restart()`, the only place this project calls it.
  Only covers reboots this project's own code actually triggers, a real
  power button hold or cold boot has no running code to do any muting
  first, that case is not fixable in software, worth knowing if the pop
  still happens outside the Apply button specifically.

- **Ext+USB intermittent pops, "many min apart... every few sec when
  playing... overloads quicker with a ton of notes", not yet fixed, but
  a concrete lever already exists**: `usb_audio_out::writeBlock()`
  (scale loop plus `tud_audio_write()`, confirmed a fast, non blocking
  ring buffer push, not itself likely to stall) runs in the same audio
  task, same iteration, as ModuleAudio's blocking `device.play()`. Even
  cheap extra per block work, added on top of an already marginal render
  under heavy polyphony (the known, largely mitigated but not eliminated
  6 voice cost), is plausibly what occasionally tips a block over its
  time budget into a ModuleAudio underrun, matching both "more notes
  makes it worse" and why the USB feed itself sounds clean (nothing
  dropped there, the pop is specifically ModuleAudio's write missing its
  window). This is exactly what Out Buffer (twenty seventh pass) was
  built for: worth the user trying a depth of 2-3 there directly against
  this specific symptom before any more code changes, not yet confirmed
  either way.

Nothing in this pass confirmed on hardware yet except by report (the bugs
being fixed), the fixes themselves are new. `LOOPANINI_VERSION` bumped to
v0.1.11.

**Update 2026-09-26 (thirty first pass): the real internal speaker bug,
task pinning was real but not the whole story.** User reported the
glitching got MORE consistently ring mod like after the pinning fix, not
less, the opposite of what a scheduling contention fix should do if it
were the whole story, a genuinely useful signal, not just "still broken".

Traced properly instead of tuning the same knob again: re-read
`loopanini.ino`'s `audioTask()` with fresh eyes. `audio_io::writeBlock()`
failing already had a comment explaining its `vTaskDelay(1)` exists
because "nothing else in this loop ever waits" for anything on a
successful write, that line is the tell. ModuleAudio's `device.play()` is
the ONLY thing in this whole loop that ever blocks, which means it is
also the only thing that has EVER paced this audio task to real time.
`M5.Speaker.playRaw()` never blocks. So with `ANALOG_INTERNAL` active,
nothing paces this task at all: it renders, mixes, and calls
`writeInternal()` as fast as render and mix cost allow, typically under
1ms per block per this project's own `render_us` logging, against a
2.7ms real time budget (128 samples at 48kHz), meaning it was racing
2-3x realtime. No number of buffers and no core pinning fixes an
unpaced producer outrunning a fixed rate consumer, that is a hard limit,
not a tuning problem, pinning only made the consumer's OWN 48kHz pace
more reliable, which is exactly why the resulting periodic overrun
pattern got MORE regular instead of less, the signal was correctly read.

Fixed at the actual root: `writeInternal()` now waits for a genuinely
free buffer slot before proceeding, bounded to 4000us (a little over one
block's nominal 2.7ms) so a truly stuck background task can never hang
the audio task indefinitely, still falls through to the existing "drop
rather than corrupt" and the caller's own `vTaskDelay(1)`/`write_failures`
handling if that bound is ever hit for real. This restores the exact same
real time pacing `device.play()`'s blocking already gives the ModuleAudio
path for free, just built explicitly for the path that never had it.
`write_failures` (already logged once a second) should read at or near 0
in Int mode after this, same as Ext always has, worth checking on
hardware as direct confirmation this is actually fixed, not just
quieter. `LOOPANINI_VERSION` bumped to v0.1.12.

**Update 2026-09-26 (thirty second pass): Int+USB was browning out the
board, a real power problem, plus one more attempt at the Ext reboot
pop.** User reported the aux reboot pop still happens, right before the
new session's own boot sequence starts, accepted that this may not be
fixable ("oh well if we cant"), and separately, something much more
serious: Int+USB now "loudly burps and then restarts", repeatedly,
stuck in a loop.

The boot log made this diagnosable, not just describable: `last reset
was: power on`, specifically, not brownout, not panic, not a watchdog,
`resetReasonName()` (loopanini.ino) gives each of those its own distinct
string and this was genuinely `ESP_RST_POWERON`. That reset reason only
fires from a voltage collapse deep enough to trip the chip's actual
power-on-reset circuit, a much harder drop than the brownout detector's
own (gentler, software configured) threshold, so this is a real current
draw problem, not a code crash. `beginInternalSpeaker()` had set `M5.
Speaker.setVolume(255)`, its own max, mirroring ModuleAudio's own "max
volume for bring up" comment without noticing CoreS3's onboard speaker
amp is a much smaller, more power constrained part than a full external
codec, especially stacked with USB's own draw and everything else this
board runs. Turned down to 80, clearly audible, well under max, a
genuine fix for a real hardware constraint, not a preference.

Worth naming honestly: the previous pass's pacing fix (writeInternal
waiting for a real free slot) most likely made this worse, not better,
by removing a mechanism that was accidentally hiding it. Frequently
dropped, choppy audio has a lower average duty cycle than smooth,
continuous audio at the same peak level, so fixing the audio quality bug
plausibly raised the average current draw enough to cross a line that
had, until then, gone unnoticed. Both fixes are correct and both were
needed, worth remembering that a fix can genuinely make a DIFFERENT,
previously masked problem more visible rather than introducing a new one
of its own.

Ext reboot pop: widened `muteBeforeReboot()`'s settle delay from 50ms to
150ms, still muting first (device.setMute(true) for Module, M5.Speaker.
stop() for Internal). Longer settle time is the only lever a register
level mute call actually has, an analog output stage's own settling
behavior is not something software can force faster. If this still
doesn't clear it, this codec most likely simply lacks a dedicated mute
relay or soft start circuit, and some residual pop on a hard reset may
be a real hardware limitation rather than something fixable in software,
consistent with the user's own "oh well" already covering that outcome.

Not yet confirmed on hardware. `LOOPANINI_VERSION` bumped to v0.1.13.

**Update 2026-09-26 (thirty third pass): the boot loop was a real bug in
the previous pass's own wait, not the volume/power issue at all.** User
reported the exact same bad audio plus a hard boot loop, screen never
loading. The boot log's own reset reason gave the real cause directly:
`TASK WATCHDOG, a task starved the idle task`, not brownout, not power
on this time, a completely different mechanism than the thirty second
pass diagnosed.

The bug was in the thirty first pass's own fix: `writeInternal()`'s
bounded wait used `delayMicroseconds()`, which is a tight cycle counter
spin, it does not yield to the FreeRTOS scheduler at all. Running on core
0 (`LOOPANINI_AUDIO_TASK_CORE`), that spin meant core 0's own idle task,
whose only job is feeding that core's task watchdog, never got scheduled,
exactly what the reset reason says happened. Ironic given loopanini.ino's
own `audioTask()` already has a comment explaining this exact failure
mode next to its `vTaskDelay(1)` on a failed write, the same mistake got
made again one layer down instead of reusing the lesson already written
down.

Fixed by switching the wait to `vTaskDelay(1)` in a bounded loop (4 ticks,
matching the old ~4ms bound at this project's ~1ms tick rate, same
assumption `audioTask()`'s own `vTaskDelay(1)` already relies on).
`vTaskDelay` actually yields, so the idle task, the scheduler generally,
and possibly the internal speaker's own background task (now pinned to
the other core, thirty first pass) all get a real chance to run during
the wait, instead of core 0 being tied up spinning uselessly. Needed new
`#include <freertos/FreeRTOS.h>` / `<freertos/task.h>` in audio_io.cpp
for `vTaskDelay`.

Also addressed: user felt muting needs to happen further into boot, since
the burp comes a while in, only after also confirming the reduced volume
(thirty second pass) made it quieter. Once this pass's actual fix lands,
the burp-then-crash-loop this was describing should stop happening at
all, that specific burp was the crash itself producing noise mid glitch,
not a graceful shutdown transient `muteBeforeReboot()` could ever have
reached, that function only ever runs from the Audio Out picker's
deliberate Apply button, never from an unplanned watchdog reset. If a
quieter, non crashing pop still remains after this fix, on either
transport, that would be the same already known, not fully solved reboot
pop (thirty first/thirty second passes), a real but separate issue.

Not yet confirmed on hardware, this is the most urgent fix in this
session to verify, it was blocking the screen from loading at all.
`LOOPANINI_VERSION` bumped to v0.1.14.

**Update 2026-09-26 (thirty fourth pass): the internal speaker wait
redone a third time, with a semaphore instead of a poll.** User confirmed
Ext+USB stayed clean, narrowing this specifically to whichever mode has
Int active, and that the crash was gone but the garbling ("bitcrush
ringmod") was not, after the vTaskDelay(1) fix. Useful data: it ruled out
USB itself as a cause (Ext+USB clean means USB isn't inherently the
problem) and confirmed this task's own cadence, not USB, is the shared
mechanism when Int is active (matches: this same audio task feeds both
paths every block, so whatever stalls its iteration stalls both).

Reasoned through why vTaskDelay(1) polling, while a genuine fix for the
crash, was never going to be precise enough on its own: this project's
poll tick is roughly 1ms, the target block period is roughly 2.7ms
(AMY_BLOCK_SIZE at LOOPANINI_SAMPLE_RATE), a ratio coarse enough that
most blocks need at least one tick's wait, and a wait that lands a little
short or a little long of the real period, tick after tick, accumulates
into exactly the kind of periodic drift "ring mod" describes. Polling on
a timer was never going to track a hardware clocked consumer precisely,
regardless of the poll interval chosen.

Replaced with what the library's own docs actually describe: a
`SemaphoreHandle_t`, given once by `onSpkBufferReleased` (the real time
reference, whichever instant the background task is actually done with a
buffer) and taken by `writeInternal`'s wait, still bounded by an absolute
deadline (4ms) so a genuinely stuck background task can never hang the
audio task. One real subtlety, documented in the code: a give is not
necessarily for the exact slot index `writeInternal` is waiting on (3
buffers rotate), so `spkFree[i]` is re-checked after every take rather
than trusting one give to mean this specific index is ready.

If this still garbles Int specifically, worth checking on hardware next:
`write_failures` (already logged once a second) climbing would mean
blocks are still genuinely being dropped, still a pacing problem, worth
more of this same investigation; `write_failures` staying near 0 while it
still sounds bad would point somewhere else entirely, most likely
something in the actual `playRaw()` call parameters (stereo interleaving,
repeat count) rather than timing, worth checking against a working
example next rather than tuning this same wait a fourth time.

Not yet confirmed on hardware. `LOOPANINI_VERSION` bumped to v0.1.15.

### Status 2026-09-23: aux in is live

`audio_io::readBlock()` is now called every audio block, so EXT (ModuleAudio's
mic/line jack) is a real signal for the first time, not just a mixer
placeholder. EXT has its own fader, mute, solo and meter, same as INT. INT and
EXT are summed into the loop record tap independently, gated by each column's
own loop-record button, so either or both can feed the looper. Stutter gained
a fourth target, Aux (Config's Stutter Track list is now Looper, Synth, Main,
Aux), so a live stutter works on the incoming aux signal the same way it
already did on the synth, the loop, and the main mix. **Not yet confirmed on
hardware** whether calling readBlock and writeBlock both every block keeps
pace, ModuleAudio's ES8388 is a duplex codec so this should just work, but
only a real run proves it: watch the STAGE lines and the `audio:` heartbeat
for write failures or a rate that isn't the healthy ~188 blocks per 2 s line.

## Recording to SD

A later, bolt-on addition, not something to build early or design around
now. It naturally wants to tap the master mix, the same signal already
going to analog and USB out, and that's only a well-defined single point
once the Mixer above actually exists, so it belongs after that, not before.

The one thing worth carrying from day one so this stays bolt-on-able:
the audio task only ever pushes rendered blocks into non-blocking queues,
never blocks waiting on a consumer, exactly the discipline the USB MIDI
host fix in Core and Task Split established. SD recording is just one more
consumer of that same pattern, a core 1 task draining a ring buffer queue
that the audio task pushes into and never blocks on. A 48 kHz stereo stream
is around 192 KB/s, trivial for SD sequential write throughput, the actual
risk isn't throughput, it's SD cards occasionally blocking for tens of
milliseconds during their own internal housekeeping, unpredictably, the
same class of hazard that broke the USB MIDI host task before it got its
own core 1 task. The queue is what absorbs those spikes.

File writes get flushed periodically rather than after every write, buying
back SD card longevity and write throughput margin at the cost of losing
up to that flush interval's worth of audio on a power failure. Deliberate
and fine, not worth engineering around.

## Comp

Modeled on Reaper's LOSER master limiter
(https://github.com/Samelot/Reaper/blob/master/Effects/LOSER/masterLimiter),
which is a lookahead peak limiter:

- Threshold and ceiling as separate controls, the ceiling is the hard output
  cap, the threshold is where gain reduction starts.
- A lookahead circular buffer delays the audio slightly so gain reduction can
  be applied before a peak actually reaches the output, rather than reacting
  after the fact.
- Peak detection compares input level against threshold, gain factor is
  threshold divided by detected level when over threshold.
- A hold time keeps the gain reduction open briefly after a peak so it
  doesn't flutter open and closed on closely spaced transients.
- Attack and release are exponential gain smoothing toward the target gain,
  fast toward more reduction, slower back toward none.
- Makeup gain is ceiling divided by threshold, applied after limiting, output
  hard clipped to the ceiling as a final safety net.

This ports as a fixed point or float C routine, one lookahead ring buffer,
one gain state variable, attack and release coefficients computed from the
configured times.

## Synth and sampler (AMY)

- USB and DIN MIDI both route into AMY note and CC handling on core 0.
- Channel 10 is drums and one shot samples, samples are preloaded into PSRAM
  at kit load time so a repeated hi hat never touches the SD card mid
  performance. This uses AMY's RAM loaded sample mode directly, it does not
  need a custom cache built on top.
- Channels 1 to 3 are pitched synth voices or pitched samples using AMY's
  sampler with base note pitch shifting. Four channels total is the ceiling,
  matching what was asked for.
- SD card holds the kit files and is only touched at boot or on a kit change,
  loading is chunked on core 1 so it can't stall audio.

### Status 2026-09-22: SD sample cache, built and building

`src/sample_bank.cpp` reads 16 bit PCM WAV files off the SD card with the SD
library and calls AMY's own `pcm_load()` (declared in `amy.h`, the same C
function AMY's `load_sample` wire command calls internally, so this is AMY's
documented direct path, not a workaround) to allocate each sample's RAM and
fill it once. `synth_engine::begin()` sets `amy_config.ram_caps_sample =
MALLOC_CAP_SPIRAM` so that RAM is PSRAM, not AMY's small internal-RAM default
on non-Tulip/AMYboard builds. After that first load, a repeated hit plays from
PSRAM, the SD card is never touched again until a kit changes. SD comes up on
its own SPI bus with the exact pin lookup and GPIO4 fallback M5Unified's own
`Speaker_SD_wav_file` example uses (that bus is separate from the CoreS3
LCD/Module USB SPI pads in the pin budget above).

**How this compares to the Blackbox mk1 and SamplerBox**, since both do the
same "WAV files on a card become playable pads/notes" job differently:
- **1010music Blackbox** stores samples inside a *project*: each pad or track
  cell in the Blackbox's own project file references a WAV by path, and the
  unit streams from its SD card during playback (its "Cell" sample player is
  disk streaming, not a RAM bank), so it can hold far more sample data than
  RAM but a very fast repeated hit can be limited by the card's read speed.
  There is no plain "drop files in a folder, note number picks the file"
  scheme, the project file is the source of truth for what plays where.
- **SamplerBox** (the Pi/Python project) is the closer relative: one folder
  per instrument/kit, and the WAV filenames inside it encode the mapping,
  typically a leading MIDI note number (and a velocity layer suffix on some
  forks), so "36 kick.wav" plays on note 36. It fully preloads that folder
  into RAM at startup for exactly the reason we do: no disk I/O once a
  performance starts.
- **Loopanini** follows SamplerBox's convention on purpose, since it is the
  simplest correct mapping for a folder of drum one-shots and needs no
  authoring tool: `LOOPANINI_SD_KIT_DIR` (`/kits/000` by default, in
  `config.h`) is a flat folder of WAVs, each filename's leading digits are
  the MIDI note it plays on (`"36 Kick.wav"`, `"36_Kick.wav"`, `"36.wav"` all
  parse to 36), loaded into PSRAM at boot with `synth_engine::loadDrumKit()`.
  Unlike SamplerBox we route through AMY's own PCM engine (`pcm_load()` plus
  a raw `wave=PCM` osc event per hit) rather than a custom mixer, so pitch,
  looping and AMY's other PCM modes are still available per sample if we
  extend the mapping later, and drum hits sit in the same signal chain
  (mixer, limiter, stutter) as everything else in Loopanini.
- `sample_bank::loadPitchedSample(chIndex, path)` is the same PSRAM-cached
  loader for a single sample on one of channels 1 to 3 (preset numbers
  `LOOPANINI_PITCHED_PRESET_BASE + chIndex`), native pitch defaults to C4;
  not wired to any UI control yet, see below.

**What plays through the sample layer versus AMY's built in synths:** a
channel 10 note whose number has a loaded sample plays from PSRAM through a
small dedicated pool of raw oscillators (`LOOPANINI_DRUM_OSC_BASE`,
`LOOPANINI_DRUM_OSC_COUNT` in `config.h`, 8 voices of simultaneous one-shot
polyphony by default, round robin, reserved well clear of what
`default_synths` allocates for the Juno/DX7/kit synths so they never fight
over oscillators) so a rapid hi hat roll doesn't steal a voice from the
melodic synths. A channel 10 note with no loaded sample still plays AMY's own
baked in TR-808 kit as before, so a partial kit (say, only a kick and snare on
SD) is fine. `synth_engine::routeDrumNote()` is the single choke point that
decides sample layer versus AMY for a note; it is wired into both the USB
device MIDI path (`midi_io.cpp`) and the USB host MIDI path (`midi_host.cpp`)
today. **DIN MIDI is not wired yet**, since `midi_din.cpp` reads MIDI a byte
at a time and reassembling channel-voice messages from that stream needs a
small running-status parser this pass didn't add; a DIN-triggered channel 10
note currently always goes to AMY's baked in kit even if a sample is loaded
for that note.

**MIDI channel change from the AMY screen is now live**, it calls AMY's
`to_synth` to actually move the synth, tracked per UI slot so later patch and
volume edits keep addressing the right synth number after a move (this needed
a small fix mid-build: the UI's per-slot synth number has to update when
`to_synth` fires, or the next patch/volume edit would silently address the
old, now-vacated synth number).

**Not done yet, scoped out of this pass:**
- No UI for browsing or picking a kit folder, loading a pitched sample, or
  seeing what's mapped where. `LOOPANINI_SD_KIT_DIR` is compile-time only.
- Only one WAV per note (no velocity layers, no round robin per hit), and
  only one WAV per pitched channel (no keyboard zones/splits). AMY's own PCM
  engine supports more of this per preset already (loop points, `PCM_LEFT`/
  `PCM_RIGHT` for true stereo via two oscs and pan, `disk_sample` streaming
  for files too big to fit in PSRAM), extend `sample_bank` into that as
  needed rather than reinventing it.
- Real AMY parameter editing beyond patch number, MIDI channel and volume
  (filter, envelope, effects per engine) is still just the three field list
  from UI pass 2, the plan doesn't yet describe what a fuller per-engine
  parameter screen should look like, revisit once the mixer and looper are
  solid.
- 16 bit PCM (or WAVE_FORMAT_EXTENSIBLE with a PCM subformat) WAV only, mono
  or stereo. 24 bit, float, or compressed WAV are refused with a debug
  message rather than played back wrong.

## Status LEDs

Phase one: WS2812B reflects loop state color (armed, recording, overdubbing,
playing, stopped, cleared) the same states as the on screen icon, so the
performer can read status without looking at the screen. A bar position
chase or beat pulse is a reasonable phase two addition once the state
colors are solid, not part of the first pass.

## Open items to confirm during hardware bring up

- **Confirm the pin collision diagnosis on hardware.** Flash the guard build
  (DIN on, USB host off, no hardware change) and check the staged bring up log:
  every stage should read OK, and the audio should be clean. If it is, the
  SS Select CH1 = MCLK collision is confirmed as the phase 2 cause. If DIN alone
  breaks a stage, the diagnosis was incomplete and the ESP-IDF error lines in the
  log say why.
- **DIN MIDI versus USB host: resolved, both run.** USB host has priority. The
  Module USB uses SS CH2 (GPIO1) + INT CH1 (GPIO10) with Port A empty, the Unit
  MIDI is on Port B (GPIO8/9), or Port C (GPIO18/17), picked by the switch at
  the top of `src/config.h`. Still to confirm on hardware that Port B's Grove
  connector really lands on GPIO8/9, it follows M5Unified's table and the
  M-Bus pin it maps to, but the connector itself hasn't been probed.
- **Why the I2S writes failed instantly** is not established, only that they did
  and that the audio loop then spun and tripped the watchdog. See Phase 2 crash.
- DIN MIDI: the UART pair is confirmed working in direction (notes reached
  AMY). A UART on Port A, no software serial needed.
- USB MIDI direction: resolved. Device mode is always built on CoreS3's
  native USB-C port (arduino-esp32's `USBMIDI`, board USB Mode set to
  USB-OTG/TinyUSB), CoreS3 shows up as a MIDI port to a computer or DAW.
  Host mode (a class compliant USB MIDI keyboard plugged into the stack) is
  covered separately by the stacked Module USB v1.2 (MAX3421E) over SPI, see
  Hardware and Toolchain, so the native port never needs to switch roles.
  CoreS3's AXP2101/AW9523B do expose pins for a native OTG role switch, and
  a native-OTG-host bench spike is still worth a quick look out of curiosity
  since it would remove a chip if it turned out to work, but it's no longer
  load bearing, the module already gives a proven host path.
- Confirm ModuleAudio switch B behaves as documented on the actual board
  revision in hand, before relying on the external jack and internal mic
  being switchable purely in software.
- PSRAM budget math for the chosen sample rate, bit depth, and maximum loop
  length, once those numbers are picked, to make sure the drum kit cache,
  the live loop buffer, and all four bank slots fit comfortably in the 8 MB
  available.
- Whether ModuleAudio (or the jack wiring on it) exposes a jack detect
  signal. If it does, output can auto route to CoreS3's own speaker when
  nothing is connected. If not, output routing ships as a manual setting
  only, ModuleAudio, CoreS3 speaker, or an explicit auto mode is skipped.

## Build phases

1. Done, confirmed on real hardware. ModuleAudio I2S in and out working on
   switch B, AMY answering MIDI note on and off with audible output through
   ModuleAudio (startup bleep and USB MIDI notes both audible), USB MIDI
   device mode confirmed, and CoreS3's native port doubling as an
   output-only USB audio interface alongside USB MIDI on that same
   composite device. No looper yet.
2. **Done, confirmed on real hardware (2026-09-19).** Staged bring up reads OK
   with zero write failures. USB host MIDI from a class compliant keyboard
   plugged into the Module USB works, DIN MIDI runs on Port B (Unit MIDI on
   Port B is set by a switch at the top of `config.h`, not yet tested with a
   real DIN device, the RX pin has a pull-up so an empty port stays quiet).
   Two root causes were found and fixed: the pin collision (Module USB SS
   switch on ModuleAudio's MCLK) and a crash from creating the `USB` object
   with `new` (the library needs a zero-filled object). The switch labels are
   the Core row printed on the module, see the switch label note above, only
   the switches labelled G5 (SS) and G35 (INT) are ON.
3. Free length looper: record, play, overdub, stop, a working clear and
   undo. No bar counting, no arm and threshold yet, just confirm the core
   audio buffer and transport logic.
4. Arm and threshold detection with pre-roll, bar count and time signature,
   internal BPM, the double tap safe clear and arm shortcut.
5. Loop banks: the four slot buttons, bank on tap, independent play and
   stop, the clear-a-slot gesture settled here.
6. Touch UI for real: main screen, swipe to looper settings, state icon,
   abbreviated labels sized for fingers.
7. MIDI clock follow (USB and DIN), mixer screen, Comp screen and DSP.
8. SD kit loading into PSRAM, channel 10 drums with caching, channels 1
   through 3 pitched synth and samples.
9. WS2812B status colors, internal mic toggle, internal speaker output
   routing (manual, and auto if the jack detect item pans out), Ext/Int/Both
   looper input source.
10. Beat stutter screen (see Beat stutter), after the looper, mixer and comp
    exist.
11. Later and not essential: BLE MIDI host (see BLE MIDI host).
