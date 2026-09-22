# Loopanini firmware plan

This is the gospel reference for the Loopanini build. It gets written before code the
same way FIRMWARE_3_PLAN.md did for arpnmidi, because this project has more
simultaneous subsystems than that one did, audio capture, synth render, sample
cache, LEDs, and touch UI all sharing one chip, so the responsibilities and the
looper's state machine need to be nailed down before anything gets typed into
the Arduino IDE.

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
- Middle button cycles the limiter ceiling 0, -1, -2, -3, -4, -5, -6 dB and
  back to 0. The limiter is always on, at 0 dB it is a brick wall so nothing
  clips. Copy the LOSER "Simple Peak-1 Limiter" (Samelot/Reaper, Effects/LOSER,
  SP1LimiterJS). The fetched JSFX source (threshold in dB, `thresh =
  exp(dB / 8.65617)`, a smoothed peak envelope with roughly a 10 Hz low pass,
  `gain = max(envelope, thresh)`, output divided by gain) may be missing lines
  such as final makeup, so re-read the original file and reproduce it exactly
  when implemented.
  **Better candidate found, same LOSER folder: `MGA_JSLimiter`** (Michael
  Gruhn, GPL v3). Read from source: no delay line, it is not truly look ahead.
  It takes the peak of both channels, keeps two overlapping peak hold windows
  of `srate/128` samples (about 8 ms at 48 kHz), and takes the larger as the
  target envelope. Attack is instant (envelope jumps to a higher peak the same
  sample), release is a one pole decay `r = exp(-3 / (srate * max(release,
  0.05)))`, release default 200 ms. Gain is `thresh / env` times
  `ceiling / thresh` whenever the envelope is above threshold. So the output
  never exceeds the ceiling, which is what the always on brick wall needs, and
  the Mixer ceiling button (0 to -6 dB) maps directly to its Ceiling slider.
  Plan: use MGA_JSLimiter for the ceiling limiter, keep SP1 as a reference.
  It is GPL v3, keep the notice if the code is ported closely.
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
- Limiter release (ms), one global value used by every limiter instance in the
  mixer (MGA_JSLimiter release, default 200).
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
