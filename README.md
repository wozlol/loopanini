# Loopanini

Firmware for an M5Stack CoreS3: a single-track audio looper with an AMY synth and sampler, a 4-level mixer, USB audio and MIDI over USB (device and host) and DIN.

- Sketch: `loopanini/loopanini.ino`, modules in `loopanini/src/`
- Plan and hardware notes: `FIRMWARE_PLAN.md`
- Board: M5Stack CoreS3, USB Mode USB-OTG (TinyUSB), USB CDC On Boot Disabled
- Hardware: ModuleAudio (switch B), Module USB v1.2 (only the G5 SS and G35 INT switches ON), Unit MIDI on Port B
- Host tests: `tests/run_tests.sh`
