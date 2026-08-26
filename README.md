# ESP32-S3 TinyML Wake-Word Detector — Wokwi Simulation

Low-latency voice activator for edge devices. SIH 2026 prototype.

**This is a simulation. It is not hardware validation.** Read
[What the numbers mean](#what-the-numbers-mean) before quoting anything here.

---

## Architecture

```
Audio (16 kHz / 16-bit / mono)
      |
      +--> ring buffer (1.5 s, always writing)     <-- never stops, never gated
      |
   20 ms frame (320 samples)
      |
     VAD  (energy + zero-crossing rate)
      |
   +--+--+
   |     |
 silence activity
   |     |
  DROP   +--> log-mel (30 ms window, 40 bins, 50 fps)
              |
            INT8 DS-CNN (TFLite Micro, 500 ms context)
              |
          confidence smoothing -> threshold -> cooldown
              |
          KEYWORD DETECTED
              |
          read 1.0 s of PRE-EXISTING audio from the ring buffer
              |
          IMA ADPCM (4:1) in ~20 ms chunks
              |
          pre-warmed WebSocket (simulated)
              |
          simulated server -> canned partial transcript
```

### Why each piece is there

**Why ESP32-S3.** Dual-core 240 MHz LX7 with SIMD/DSP extensions and vector
instructions that `esp-nn` uses for INT8 convolution. The C3 (RISC-V, single
core, no vector unit) has no equivalent kernel path, so DS-CNN inference would
be several times slower for the same model.

**Why VAD before KWS.** The DS-CNN costs ~1.7 M MACs per inference; the VAD
costs a few hundred integer operations. Running the cheap test first means the
expensive one only runs when there is plausibly speech. The VAD is deliberately
*permissive* — its job is to reject silence cheaply, not to classify.

**Why the VAD has a hangover.** Speech contains short internal pauses (stop
consonants). Cutting the cascade off mid-word would truncate exactly the audio
the ring buffer exists to preserve. 8 frames = 160 ms.

**Why 40 mel bins.** Mel spacing concentrates resolution where speech formants
live. 40 is the standard KWS operating point — enough to separate phonemes,
few enough that the first conv layer stays small.

**Why 50 frames/sec.** A 20 ms hop is short enough to localise a word boundary
to within half a frame, and long enough that the feature rate (2,000 values/s)
stays trivial to compute.

**Why DS-CNN.** Depthwise-separable convolutions cut MACs by roughly the
channel count versus dense convolution, at a small accuracy cost. It is the
standard small-footprint KWS topology.

**Why INT8.** 4x smaller weights than float32, and `esp-nn` only has optimised
kernels for INT8 on the S3. Float inference would be both larger and slower.

**Why the ring buffer is 1.5 s.** By the time the KWS is confident, the wake
word is already ~300–500 ms in the past, and the user is usually already
speaking their command. 1.5 s covers the run-up, the word, and the start of
the utterance. **The trigger reads the buffer; it does not start it.** A
system that begins recording on trigger has already lost the word.

**Why IMA ADPCM and not Opus.** Opus compresses far harder but costs roughly an
order of magnitude more CPU and adds algorithmic delay. This link crosses a LAN
to a Raspberry Pi. Bandwidth is not the constraint; latency and CPU are. ADPCM
is a fixed 4:1 (256 kbps -> 64 kbps) for a handful of operations per sample.

**Why the WebSocket is pre-warmed.** A cold DNS + TCP + TLS handshake is
typically 100–300 ms on Wi-Fi, which would consume the entire 200 ms budget
before a single audio byte moved. The connection is established at boot and
held open.

---

## What the numbers mean

Three labels appear throughout the serial output, and they are not
interchangeable.

| Label | Meaning |
|---|---|
| `MEASURED IN WOKWI` | Read from a runtime API during simulation. |
| `THEORETICAL / CALCULATED` | Arithmetic, clearly shown. Not observed. |
| `REQUIRES REAL ESP32-S3 VALIDATION` | The simulation cannot determine this. |

### Memory numbers are trustworthy

ESP-IDF heap accounting is real bookkeeping, not emulation. `heap_caps_*`
figures reproduce on hardware. The ring buffer measuring 48,128 B against a
calculated 48,000 B (+132 B heap block overhead) is a fact, not an estimate.

### Timing numbers are inflated, and the firmware says by how much

Wokwi's **clock** is self-consistent: `esp_timer` and the Xtensa cycle counter
agree at exactly 240.0 MHz. Wokwi's **instruction cost model** is not. A
calibration loop of known cost (5 instructions, verified by `objdump`) runs at
~136 cycles/iteration instead of the ~5–7 real silicon would take.

**Every microsecond figure is therefore inflated by roughly 19–27x**, and is
printed as a measurement plus an estimated band. The band is an
order-of-magnitude sanity check, not a hardware result.

### DS-CNN inference is pessimistic for a *second*, independent reason

`esp-nn` ships hand-written ESP32-S3 assembly kernels that use S3 SIMD/DSP
instructions. **Wokwi does not emulate those instructions** — `Invoke()` faults
with `IllegalInstruction`. This build is therefore forced to
`CONFIG_NN_ANSI_C=y`, i.e. reference C kernels.

```
DS-CNN: 1,697,344 MACs
  ANSI-C reference @ ~15 cyc/MAC    -> ~106 ms   <- what Wokwi can run
  esp-nn assembly  @ ~0.75 cyc/MAC  -> ~5.3 ms   <- what hardware would run
```

The measurement validates the ANSI-C row only. **Remove
`CONFIG_NN_ANSI_C=y` from `sdkconfig.defaults` before flashing real hardware.**

### The latency figure is not the handoff figure

The 100-trial latency measurement covers ring extraction + ADPCM encode +
framing + simulated server receive. It **excludes** Wi-Fi transmit time, TLS
record overhead, network RTT, server queueing and real ASR compute — every term
that actually decides whether 200 ms is met. Treat it as a floor on the
on-device contribution.

### The cascade rate describes the test script, not a room

The synthetic Case D sequence is ~60% speech-like by construction. A real
always-on device sits in silence most of the time and would reject far more.
Conversely, real rooms contain fans, traffic and background TV that have
speech-like energy *and* speech-like ZCR, which the synthetic "noise" case does
not. **The measured activation rate is an artifact of the input mix.** The
number worth reading from that test is wake-word frame recall.

### The model is untrained

Random weights. Arena, footprint, shapes and latency are real. Scores are
noise. **No accuracy, false-accept or false-reject claim can be made.**
See `model/README.md`.

---

## WOKWI LIMITATIONS

```
This simulation does not constitute final physical-hardware validation.
Microphone/I2S behaviour and timing must be validated on a real ESP32-S3.
```

Specifically, the following are **not** exercised at all:

- **Microphone / I2S.** Audio is synthetic (`main/audio_sim.cpp`). There is no
  capture path, no DMA, no clock drift, no AGC, no mic self-noise.
- **Wi-Fi and TLS.** No association, no handshake, no socket, no RAM cost. A
  real Wi-Fi + mbedTLS stack costs tens of KB that the memory report below
  does **not** include.
- **ESP32-S3 SIMD/DSP instructions.** Not emulated; see above.
- **Power.** No current measurement is possible.
- **sherpa-onnx ASR.** Not integrated. The "partial transcript" is a canned
  string chosen to look like the real thing in a demo. It is not recognition.

---

## Layout

```
.
├── wokwi.toml                  Wokwi CLI config (firmware + elf paths)
├── diagram.json                ESP32-S3 devkit + serial monitor
├── sdkconfig.defaults          esp32s3, 240 MHz, no PSRAM, ANSI-C NN kernels
├── run_benchmark.sh            clean build + full simulation run
├── model/
│   ├── gen_model.py            builds the INT8 DS-CNN
│   ├── ds_cnn_int8.tflite      UNTRAINED placeholder
│   └── README.md               what the model does and does not prove
└── main/
    ├── main.cpp                phase-by-phase benchmark driver
    ├── config.h                geometry, static_assert'd
    ├── metrics.{h,cpp}         heap checkpoints + CPU calibration
    ├── ring_buffer.{h,cpp}     1.5 s circular pre-roll
    ├── audio_sim.{h,cpp}       SIMULATION INPUT — not a microphone
    ├── vad.{h,cpp}             energy + ZCR gate
    ├── features.{h,cpp}        log-mel, esp-dsp FFT
    ├── kws.{h,cpp}             TFLM interpreter + decision logic
    ├── adpcm.{h,cpp}           IMA ADPCM
    └── network.{h,cpp}         SIMULATED uplink + server
```

`main/` is used instead of the `src/` + `include/` split because this is an
ESP-IDF project, not Arduino — IDF components expect sources beside their
`CMakeLists.txt`.

---

## Running it

### Headless (fastest, what produced the numbers)

```bash
export WOKWI_CLI_TOKEN=wok_...          # https://wokwi.com/dashboard/ci
./run_benchmark.sh
```

Requires ESP-IDF v5.4 (`~/esp/esp-idf`) and `wokwi-cli`
(`curl -L https://wokwi.com/ci/install.sh | sh`).

### Visually on wokwi.com

1. Build once (`./run_benchmark.sh`, or `idf.py build` + the `merge_bin` step).
2. Open the ESP32 custom-application template:
   <https://wokwi.com/projects/305457271083631168>
3. <kbd>F1</kbd> -> **Upload Firmware and Start Simulation…** -> choose
   `build/wokwi-upload.bin` (the *merged* image — the bare `kws.bin` will not
   boot, it has no bootloader or partition table).
4. Replace the template's `diagram.json` with this project's.

The full benchmark takes several minutes of simulated time and the browser is
slower than the CLI.

### In VS Code

The Wokwi VS Code extension reads `wokwi.toml` and `diagram.json` directly and
runs the local `build/` output, with GDB debugging.

---

## Self-checks

Non-trivial logic carries a runnable assertion that fails loudly:

- **Ring buffer** — writes 1.75x capacity, asserts wrap, ordering and pre-roll
  contents.
- **ADPCM** — round-trip on a swept sine, asserts bounded RMS error (it is a
  lossy codec, so equality is the wrong test).
- **Decision logic** — synthetic score ramp, asserts that low scores never
  fire, sustained high fires exactly once, a burst inside the cooldown is
  suppressed, and one after it fires. This does not depend on the untrained
  model.
- **`esp_timer` sanity** — checked against a 100 ms `vTaskDelay` before any
  timing figure is trusted.
- **`config.h`** — geometry is `static_assert`ed, so a typo fails the build.
