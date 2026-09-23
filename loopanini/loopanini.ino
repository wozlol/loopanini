// Loopanini phases 1 and 2: ModuleAudio I2S in and out on switch B, AMY
// answering MIDI note on and off with audible output, USB MIDI device mode,
// CoreS3's native USB port doubling as an output-only (device to host) class
// compliant 2 channel USB audio interface, plus two optional MIDI inputs that
// can be switched on and off at the top of src/config.h: DIN MIDI through the
// Unit MIDI (Port B by default) and USB MIDI host through the stacked Module
// USB v1.2. No looper yet. See FIRMWARE_PLAN.md at the repo root for the full
// plan and build phase list this sketch is working through, and the pin
// budget in src/config.h for how they share the stack without colliding.
//
// Board settings this sketch needs (Tools menu):
//   Board: M5CoreS3 (esp32:esp32:m5stack_cores3)
//   USB Mode: USB-OTG (TinyUSB)   -- required for USBMIDI and USBAudioCard,
//                                    see src/midi_io.h and src/usb_audio_out.h
//   USB CDC On Boot: Disabled     -- debug output goes over our own CDC
//                                    interface (src/debug_io.h), registered
//                                    alongside MIDI and Audio before the one
//                                    shared USB.begin() call. Leaving this
//                                    board option Enabled instead makes the
//                                    core auto-start a CDC-only USB
//                                    connection before setup() even runs,
//                                    and our own code adding MIDI and Audio
//                                    afterward then forces a disruptive
//                                    re-enumeration mid-boot that kills the
//                                    Serial Monitor connection right as USB
//                                    MIDI comes up. See src/debug_io.h.
//
// Hardware this sketch expects, see FIRMWARE_PLAN.md's Hardware section:
//   ModuleAudio v1.0, physical switch set to B.
//
// Serial Monitor (115200), type a letter and send it:
//   ?  reprint the boot summary: reset reason, pin table, stage results
//   b  reboot into the USB bootloader, then close the monitor and Upload,
//      no RESET button hold needed
//   r  plain reset

#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp32-hal-tinyusb.h>
#include <esp_system.h>
#include <esp_core_dump.h>

#include <AMY-Arduino.h>
#include <M5Unified.h>
#include <USB.h>

#include "src/audio_io.h"
#include "src/config.h"
#include "src/debug_io.h"
#include "src/midi_din.h"
#include "src/midi_host.h"
#include "src/midi_io.h"
#include "src/pin_guard.h"
#include "src/sample_bank.h"
#include "src/spi_lock.h"
#include "src/synth_engine.h"
#include "src/ui.h"
#include "src/usb_audio_out.h"

namespace {

// Written by the audio task, read from setup() and the heartbeat. 32 bit
// reads and writes are atomic on the ESP32-S3.
volatile uint32_t write_failures = 0;
volatile uint32_t blocks_rendered = 0;

esp_reset_reason_t boot_reset_reason = ESP_RST_UNKNOWN;

constexpr size_t kMaxStages = 4;
char stage_log[kMaxStages][200];
size_t stage_count = 0;

const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "external reset";
    case ESP_RST_SW: return "software reset";
    case ESP_RST_PANIC: return "PANIC, the firmware crashed";
    case ESP_RST_INT_WDT: return "INTERRUPT WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK WATCHDOG, a task starved the idle task";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "BROWNOUT, the supply dipped";
    default: return "other, see esp_reset_reason_t";
  }
}

void printBootSummary() {
  auto &out = debug_io::out();
  out.println("---- loopanini boot summary ----");
  out.printf("last reset: %s (%d)\n", resetReasonName(boot_reset_reason),
             static_cast<int>(boot_reset_reason));
  pin_guard::printTable();
  for (size_t i = 0; i < stage_count; ++i) out.println(stage_log[i]);
  out.printf("audio counters now: blocks=%lu write failures=%lu\n",
             (unsigned long)blocks_rendered, (unsigned long)write_failures);
  out.println("type ? for this summary, b to reboot into the bootloader, r to reset");
  out.println("--------------------------------");
}

void pollCommands() {
  while (debug_io::available() > 0) {
    switch (debug_io::read()) {
      case '?':
      case 'h':
        printBootSummary();
        break;
      case 'b':
        debug_io::out().println("rebooting into the USB bootloader, close this monitor and Upload...");
        delay(150);
        usb_persist_restart(RESTART_BOOTLOADER);
        break;
      case 'r':
        debug_io::out().println("resetting...");
        delay(150);
        esp_restart();
        break;
      default:
        break;
    }
  }
}

// Lets the audio task run alone for `ms`, then reports whether it stayed
// healthy: no write failures, and blocks produced at about the real time
// rate. A write that fails returns instantly instead of blocking on the
// codec's clock, which shows up as a block rate several times too fast, so
// that check catches a dead output even when nothing is printed.
void stageCheck(const char *name, uint32_t ms) {
  const uint32_t blocks_before = blocks_rendered;
  const uint32_t failures_before = write_failures;
  delay(ms);
  const uint32_t blocks = blocks_rendered - blocks_before;
  const uint32_t failures = write_failures - failures_before;
  const uint32_t expected = ms * (LOOPANINI_SAMPLE_RATE / AMY_BLOCK_SIZE) / 1000;
  const bool paced = blocks >= expected * 8 / 10 && blocks <= expected * 125 / 100;
  const bool ok = failures == 0 && paced;

  if (stage_count < kMaxStages) {
    snprintf(stage_log[stage_count], sizeof(stage_log[0]),
             "STAGE %-22s blocks +%lu (healthy is ~%lu), write failures +%lu  =>  %s", name,
             (unsigned long)blocks, (unsigned long)expected, (unsigned long)failures,
             ok ? "OK" : "PROBLEM");
    debug_io::out().println(stage_log[stage_count]);
    ++stage_count;
  }
}

// USB MIDI host lives on its own task on core 1, off the real-time audio
// core, see config.h. begin() runs here so a slow or absent module can never
// delay the audio task starting, and if the feature is disabled or its pins
// were refused there is nothing to poll, so the task just ends.
void hostMidiTask(void *) {
  if (!midi_host::begin()) {
    vTaskDelete(nullptr);
  }
  for (;;) {
    midi_host::poll();
    vTaskDelay(1);  // yield, this task has no deadline to race against
  }
}

void audioTask(void *) {
  uint32_t last_report_ms = 0;
  uint32_t last_blocks = 0;
  uint32_t last_failures = 0;
  for (;;) {
    midi_io::poll();
    midi_din::poll();
    int16_t *block = synth_engine::renderBlock();
    // Aux in (ModuleAudio's mic/line jack): read even if nothing has it
    // record-enabled or unmuted, so the meter and any live stutter on it
    // stay correct the instant either is turned on. A failed read (module
    // busy or not ready) just leaves this block's input silent, it doesn't
    // block or spin: audio_io::readBlock has the same immediate-return
    // failure behavior as writeBlock, see the comment below.
    static int16_t aux_block[AMY_BLOCK_SIZE * 2];
    if (!audio_io::readBlock(aux_block, AMY_BLOCK_SIZE)) {
      memset(aux_block, 0, sizeof(aux_block));
    }
    ui::processBlock(block, aux_block, AMY_BLOCK_SIZE);
    if (!audio_io::writeBlock(block, AMY_BLOCK_SIZE)) {
      write_failures = write_failures + 1;
      // A failed write returns immediately instead of blocking on the I2S
      // clock, and nothing else in this loop ever waits, so without this it
      // becomes a hot loop that starves core 0's idle task and trips the task
      // watchdog after 5 seconds, which reboots the board. That reboot loop
      // is what phase 2 bring up looked like.
      vTaskDelay(1);
    }
    usb_audio_out::writeBlock(block, AMY_BLOCK_SIZE);
    blocks_rendered = blocks_rendered + 1;

    const uint32_t now = millis();
    if (now - last_report_ms >= 1000) {
      last_report_ms = now;
      const uint32_t blocks = blocks_rendered;
      const uint32_t failures = write_failures;
      debug_io::out().printf("audio: +%lu blocks/s (healthy ~375), write failures +%lu (total %lu)\n",
                              (unsigned long)(blocks - last_blocks),
                              (unsigned long)(failures - last_failures), (unsigned long)failures);
      last_blocks = blocks;
      last_failures = failures;
    }
  }
}

}  // namespace

void setup() {
  boot_reset_reason = esp_reset_reason();

  auto cfg = M5.config();
  M5.begin(cfg);

  // A blank screen for the several seconds setup() takes (USB enumeration,
  // ModuleAudio, AMY, SD kit load, USB host) looks like a hang, so put
  // something up the moment the display is ready, well before any of that.
  // ui::begin() (much later, once everything is up) replaces this with the
  // real UI on its first draw.
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(3);
  M5.Display.drawString("LOOPANINI", M5.Display.width() / 2, M5.Display.height() / 2 - 12);
  M5.Display.setTextSize(1);
  M5.Display.drawString("starting...", M5.Display.width() / 2, M5.Display.height() / 2 + 20);

  // Named explicitly so the host OS and DAWs show "Loopanini," not a
  // generic default, for the device as a whole. USBMIDI's own name (set in
  // src/midi_io.cpp) covers the MIDI port specifically, some hosts show
  // that name instead of, or alongside, this one.
  USB.productName("Loopanini");
  USB.manufacturerName("loopanini");

  // All three USB interfaces register themselves before the one shared
  // USB.begin() call below, so there is exactly one enumeration with the
  // full composite descriptor, not an early CDC-only one that MIDI and
  // Audio then have to force their way into. See src/debug_io.h.
  debug_io::begin();       // our own CDC interface, in place of Serial
  midi_io::begin();        // registers the USBMIDI interface
  usb_audio_out::begin();  // registers the UAC (mic-only) interface
  USB.begin();

  // Give a Serial Monitor a moment to attach so the boot log isn't printed
  // into the void. Never waits if none shows up, and '?' reprints it later.
  debug_io::waitForHost(4000);
  debug_io::out().printf("loopanini: last reset was: %s\n", resetReasonName(boot_reset_reason));
  if (boot_reset_reason == ESP_RST_PANIC && esp_core_dump_image_check() == ESP_OK) {
    esp_core_dump_summary_t *sum = (esp_core_dump_summary_t *)malloc(sizeof(esp_core_dump_summary_t));
    if (sum != nullptr && esp_core_dump_get_summary(sum) == ESP_OK) {
      auto &o = debug_io::out();
      o.printf("PANIC task=%s cause=%u pc=0x%08x\n", sum->exc_task, (unsigned)sum->ex_info.exc_cause,
               (unsigned)sum->exc_pc);
      o.print("PANIC backtrace:");
      for (uint32_t i = 0; i < sum->exc_bt_info.depth && i < 16; i++) o.printf(" 0x%08x", (unsigned)sum->exc_bt_info.bt[i]);
      o.println();
    }
    free(sum);
  }
  debug_io::out().println("loopanini: bringing up ModuleAudio...");

  // Audio claims its pins first inside audio_io::begin() and wins every
  // conflict, see src/pin_guard.h.
  if (!audio_io::begin()) {
    // No retry: re-running the I2S init on an already started driver would
    // just fail differently. Report, and keep serving serial commands so a
    // reset or a bootloader reboot is still one keystroke away.
    for (;;) {
      debug_io::out().println(
          "ModuleAudio not found. Check the physical switch is set to B and "
          "the module is seated on the stack. Type r to reset, or b for the bootloader.");
      for (int i = 0; i < 10; ++i) {
        pollCommands();
        delay(100);
      }
    }
  }
  debug_io::out().println("loopanini: ModuleAudio ready.");

  synth_engine::begin();
  debug_io::out().println("loopanini: AMY started.");

  // SD card is optional: no card, no kits folder, or nothing matching just
  // means channel 10 stays on AMY's baked in drum kit and channels 1-3 stay
  // on Juno/DX7/piano patches, see FIRMWARE_PLAN.md's Synth and sampler.
  if (sample_bank::begin()) {
    synth_engine::loadDrumKit(LOOPANINI_SD_KIT_DIR);
  }

  // Staged bring up: audio runs alone first, then each optional input is
  // enabled in turn with a health check after each, so if the audio breaks
  // the log names the stage that broke it.
  xTaskCreatePinnedToCore(audioTask, "audio",
                           LOOPANINI_AUDIO_TASK_STACK_BYTES / sizeof(StackType_t),
                           nullptr, LOOPANINI_AUDIO_TASK_PRIORITY, nullptr,
                           LOOPANINI_AUDIO_TASK_CORE);
  stageCheck("audio + USB MIDI only", 2000);

  midi_din::begin();
  stageCheck("after DIN MIDI", 2000);

  xTaskCreatePinnedToCore(hostMidiTask, "usb_host_midi",
                           LOOPANINI_USB_HOST_TASK_STACK_BYTES / sizeof(StackType_t),
                           nullptr, LOOPANINI_USB_HOST_TASK_PRIORITY, nullptr,
                           LOOPANINI_USB_HOST_TASK_CORE);
  stageCheck("after USB host", 3000);

  printBootSummary();
  ui::begin();
}

void loop() {
  ui::update();
  pollCommands();
  delay(5);
}
