# Porting PianoCraft physical models to the M-VAVE FM-1: source review and reduced-port plan

## Scope and evidence conventions

This review combines target primary sources with inspection of PianoCraft's local DSP implementations. It separates:

- **Source-confirmed** facts, backed by the linked vendor repository, firmware source, community hardware layer, or local source at the cited revision.
- **Engineering hypotheses**, which are proposed reductions and still require an on-device render and listening test.

The linked [`zvenson/fm1-build`](https://github.com/zvenson/fm1-build/blob/85a003ab455b1c074b0bd6d7c282c096a464b973/README.md) is only a reusable build-and-attestation workflow. It supplies no synth architecture or hardware specification. Following it to the [FM-1 Firmware Hub](https://thefirmwarehub.com/) leads to the two useful, distinct development paths: M-VAVE's official [`fm1_open`](https://github.com/m-vave/fm1_open/tree/4c7496daac04a95ff10d1afdcc8f9ead2dbabc41), and the community [`Felucca`](https://github.com/hugelton/Felucca/tree/406b328ee43ffb8fd3d2eb4cce1a741db4a4cea1) / [`fm1-base`](https://github.com/charlesvestal/fm1-base/tree/02f422c7d9de121ca099bb2fdebe5c035f59e8ad) sources.

**Recommendation:** build a new, reduced standalone firmware engine on `fm1-base`/`felucca-bsp`, starting with harp. Do not promise a direct port of the CLAP piano. Felucca already demonstrates fixed-point physical models with 12 modes and three voices per part on this target; that is evidence for a small modal engine, not for PianoCraft's full modal counts. This existing target proof point is why harp ranks ahead of the arithmetically simple but nonlinear reed models.

A local 64-bit host `sizeof` check provides reduction context, not an FM-1 memory prediction: the current desktop objects occupy 66,216 bytes (flute), 66,344 (reed), 290,000 (harp), and 2,419,672 (piano `InstrumentModel`, before its voice state). Horn's 4,672-byte object and strings' 60,280-byte object exclude their delay pools; at 48 kHz and full configured capacity, the source allocates 128 KiB and 4 MiB respectively. These figures come from the capacities and storage in [`src/engine`](../src/engine) and [`src/model/InstrumentModel.hpp`](../src/model/InstrumentModel.hpp). They reinforce the need for float storage, fewer voices and modes, and precomputed coefficients rather than an unchanged engine port.

## 1. Target facts from primary sources

### 1.1 Firmware, SDK, and extension mechanism

**Source-confirmed.**

- M-VAVE's [FM1 Open V14 README](https://github.com/m-vave/fm1_open/blob/4c7496daac04a95ff10d1afdcc8f9ead2dbabc41/README.md) describes eight tracks and an open application with a binary SDK. Its [build contract](https://github.com/m-vave/fm1_open/blob/4c7496daac04a95ff10d1afdcc8f9ead2dbabc41/sdk/synth.json) sets `sealed_audio_api: true`; the application calls the library's `fm1_engine_render`, while the app mixes external audio ([`app/board/audio.c`](https://github.com/m-vave/fm1_open/blob/4c7496daac04a95ff10d1afdcc8f9ead2dbabc41/app/board/audio.c), [`fm1_engine_control.h`](https://github.com/m-vave/fm1_open/blob/4c7496daac04a95ff10d1afdcc8f9ead2dbabc41/sdk/include/fm1_engine_control.h)). This revision therefore has no documented source-level replacement point for the instrument renderer.
- [Felucca](https://github.com/hugelton/Felucca/blob/406b328ee43ffb8fd3d2eb4cce1a741db4a4cea1/README.md) is actual open FM-1 synthesizer firmware. [`fm1-base`](https://github.com/charlesvestal/fm1-base/blob/02f422c7d9de121ca099bb2fdebe5c035f59e8ad/DESIGN.md) is a beta, MPL-2.0 application platform above [`felucca-bsp`](https://github.com/hugelton/felucca-bsp/tree/cd76d43ab4b94d6da9a8f1f24f3f4446135196b7): an app supplies `app_render` and the platform supplies audio, panel, MIDI, storage, host simulation, browser emulation, packaging, and installation. Its [template](https://github.com/charlesvestal/fm1-base/blob/02f422c7d9de121ca099bb2fdebe5c035f59e8ad/template/app.c) is the cleanest starting point for different DSP.
- These are whole-firmware source builds, not CLAP plugins. Felucca's [build guide](https://github.com/hugelton/Felucca/blob/406b328ee43ffb8fd3d2eb4cce1a741db4a4cea1/BUILDING.md) produces an application, update loader, and installable `.fwsc`, using JieLi's PI32v2 clang toolchain and the AC79 SDK at `AC79NN_SDK_V1.2.1_2023-12-13`. The shared workflow can attest that a package came from a source commit; its README explicitly says this is not a safety review.

### 1.2 SoC, CPU, FPU, and clock

**Source-confirmed.**

- Package and reverse-engineering evidence identifies the family as **JieLi AC791N/WL82 with PI32v2**, not an ARM target ([AL-255/FM-1-RE](https://github.com/AL-255/FM-1-RE/blob/ec832f281ca165a7d5e0722985f3acb1465046d5/README.md)). JieLi's [official AC79 SDK README](https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK/raw/AC79NN_SDK_V1.2.1_2023-12-13/README.md) specifies two DSP cores, single-precision floating point, and 578 KB on-chip SRAM for the family.
- Community register/clock measurements in [`felucca-bsp/chip/fm1_clock.h`](https://github.com/hugelton/felucca-bsp/blob/cd76d43ab4b94d6da9a8f1f24f3f4446135196b7/chip/fm1_clock.h) found the stock FM-1 boot configuration at **360 MHz**, above the SDK family's documented 320 MHz table. Felucca can use a second core, but it is optional and off by default ([`felucca.c`](https://github.com/hugelton/Felucca/blob/406b328ee43ffb8fd3d2eb4cce1a741db4a4cea1/firmware/src/felucca.c)); the BSP records device-dependent second-core failure handling in [`docs/EXPERIMENTAL.md`](https://github.com/hugelton/felucca-bsp/blob/cd76d43ab4b94d6da9a8f1f24f3f4446135196b7/docs/EXPERIMENTAL.md).
- Floating point is available in the official development path: M-VAVE compiles with `-mfprev1` in [`sdk/Build.ps1`](https://github.com/m-vave/fm1_open/blob/4c7496daac04a95ff10d1afdcc8f9ead2dbabc41/sdk/Build.ps1), and both its [pluck engine](https://github.com/m-vave/fm1_open/blob/4c7496daac04a95ff10d1afdcc8f9ead2dbabc41/third_party/synth/pluck.c) and the fm1-base template use `float`. Felucca's [`phys_dsp.c`](https://github.com/hugelton/Felucca/blob/406b328ee43ffb8fd3d2eb4cce1a741db4a4cea1/firmware/src/phys_dsp.c) instead provides a proven all-fixed-point route. Its introductory “without an FPU” wording should not be treated as the hardware specification.

### 1.3 Audio architecture and block timing

**Source-confirmed.**

- The official SDK fixes **44,100 Hz, stereo, 128-frame blocks** in [`fm1_block.h`](https://github.com/m-vave/fm1_open/blob/4c7496daac04a95ff10d1afdcc8f9ead2dbabc41/sdk/include/fm1_block.h), with signed Q23 instrument output. Its board header declares 24-bit stereo ALNK0/I2S DMA and a hardware rate of 44,118 Hz ([`app/board/fm1_audio.h`](https://github.com/m-vave/fm1_open/blob/4c7496daac04a95ff10d1afdcc8f9ead2dbabc41/app/board/fm1_audio.h)).
- Felucca owns the same external-codec output path directly: 24-bit left-justified stereo in 32-bit words, double-buffered DMA ([`felucca-bsp/boards/fm1/fm1_audio.h`](https://github.com/hugelton/felucca-bsp/blob/cd76d43ab4b94d6da9a8f1f24f3f4446135196b7/boards/fm1/fm1_audio.h)). Its [audio ISR](https://github.com/hugelton/Felucca/blob/406b328ee43ffb8fd3d2eb4cce1a741db4a4cea1/firmware/src/audio.c) renders 128-frame halves, nominally 2.9 ms, in 32-sample subblocks; its 24 MHz / 544 timing gives approximately 44,117.6 Hz.
- No source reviewed establishes the external codec's exact part number. That does not block a DSP experiment because both official and community firmware already use the digital interface.

### 1.4 RAM, flash, and storage

**Source-confirmed.**

- The AC79 family has 578 KB SRAM, but the usable budget is linker- and feature-dependent, not a single free 578 KB heap. `fm1-base` states that the FM-1 has **no external RAM** and budgets **581 KB flash for the app, 96 KB for `.data`/`.bss`, and a 336 KB large-buffer pool** in a normal build ([README, “What each costs”](https://github.com/charlesvestal/fm1-base/blob/02f422c7d9de121ca099bb2fdebe5c035f59e8ad/README.md)). Bluetooth and especially Wi-Fi consume substantial portions.
- Felucca's current [linker map](https://github.com/hugelton/Felucca/blob/406b328ee43ffb8fd3d2eb4cce1a741db4a4cea1/firmware/app.ld) instead allocates 72 KiB RAM plus a 360 KiB pool and a 581,564-byte XIP application region. The BSP describes the stock flash as 1 MiB and larger-flash support as untested ([`docs/EXPERIMENTAL.md`](https://github.com/hugelton/felucca-bsp/blob/cd76d43ab4b94d6da9a8f1f24f3f4446135196b7/docs/EXPERIMENTAL.md)).
- Persistent writes are control-side operations and can silence audio; they do not belong in the renderer ([official `fm1.h`](https://github.com/m-vave/fm1_open/blob/4c7496daac04a95ff10d1afdcc8f9ead2dbabc41/sdk/include/fm1.h)). Model tables should therefore be generated at build time or loaded before audio starts.

### 1.5 Polyphony, parameters, and MIDI

**Source-confirmed.**

- **Polyphony is firmware-defined, not a hardware constant.** Felucca declares eight voices shared across four parts in [`core.h`](https://github.com/hugelton/Felucca/blob/406b328ee43ffb8fd3d2eb4cce1a741db4a4cea1/firmware/src/core.h), but its PHYS engine limits each part to three voices in [`eng_phys.c`](https://github.com/hugelton/Felucca/blob/406b328ee43ffb8fd3d2eb4cce1a741db4a4cea1/firmware/src/eng_phys.c). M-VAVE's open instrument declares eight tracks and `GROOVE_VOICES = 8` ([`app/instrument.h`](https://github.com/m-vave/fm1_open/blob/4c7496daac04a95ff10d1afdcc8f9ead2dbabc41/app/instrument.h)); these are not budgets for a new engine.
- Felucca's PHYS engine exposes eight 7-bit engine parameters, four mapped to K1–K4, with velocity-sensitive excitation. Its PHYS DSP computes coefficients once per 32-sample block and uses 12 fixed-point SVF modes ([`phys_dsp.c`](https://github.com/hugelton/Felucca/blob/406b328ee43ffb8fd3d2eb4cce1a741db4a4cea1/firmware/src/phys_dsp.c)). This is the closest existing target proof point for PianoCraft's two-pole modes.
- The community platform exposes 27 keys, 14 buttons, seven encoders plus MASTER, USB MIDI, TRS MIDI input, and optional BLE MIDI in [`plat.h`](https://github.com/charlesvestal/fm1-base/blob/02f422c7d9de121ca099bb2fdebe5c035f59e8ad/include/plat.h). There is no physical TRS MIDI output. MIDI arrives as ordinary 7-bit channel messages, not sample-accurate CLAP automation; a custom firmware must define its own small parameter map.
- The official API accepts DIN, USB, and BLE input, with short-message and SysEx bounds documented in [`fm1.h`](https://github.com/m-vave/fm1_open/blob/4c7496daac04a95ff10d1afdcc8f9ead2dbabc41/sdk/include/fm1.h). Those official track/channel defaults should not be imposed on an independent firmware.

## 2. What remains unknown

- **Exact SoC variant/package:** AC791N/WL82 is established; a more specific AC7911 variant is not. Do not copy variant-specific SDRAM or pin claims into the port.
- **Real-time throughput:** no cited source benchmarks PianoCraft's algorithms, FPU-versus-fixed-point cost for these modes, or sustainable polyphony of a new engine. The 360 MHz figure alone does not provide a cycle budget.
- **Second-core benefit and reliability:** treat it as a later experiment, not initial capacity. Felucca and the BSP both document fallback and unit-specific failures.
- **Full-piano feasibility and table size:** no evidence shows that the current hammer solver, all transverse/coupled/phantom modes, roughly 70 soundboard modes, sympathetic bank, and target polyphony fit together.
- **A loadable plugin ABI:** the reviewed sources demonstrate replacement firmware and source-level engines, not dynamically loaded CLAP-style modules.
- **Measured audio quality:** this review did not run an FM-1 or measure its codec, underruns, or output noise.

## 3. Portability assessment — engineering hypotheses

A direct CLAP/C++ port is the wrong unit of work. The useful portable assets are the calibrated coefficients and reduced algorithms: two-pole resonators, delay-line/waveguide pieces, excitation tables, and block-rate parameter mappings. Use static voice/storage ownership, precomputed coefficients, and no allocation or flash access in audio.

The following are **not credible as an unchanged first port**: desktop double-precision solver code; per-sample hysteretic hammer iterations; the complete unison, longitudinal/phantom, soundboard, and sympathetic banks at desktop polyphony; and sample-accurate CLAP automation. “Not credible unchanged” is an engineering judgement, not proof that no highly optimized variant could ever run.

A sensible first firmware disables radio features, uses one core, and makes polyphony adaptive. Existing Felucca PHYS proves a useful floor—12 modes, three voices per part, fixed-point arithmetic—but does not prove the cost of PianoCraft's measured coefficients or more elaborate excitation.

## 4. Reduced-candidate ranking — engineering hypotheses

1. **Harp — best first candidate.** Use 8–12 strongest modes per note, 2–4 voices, a short plucked excitation, and no body initially. It maps directly onto the target's demonstrated modal/string techniques. Add at most three sympathetic strings after the basic voice passes.
2. **“Pocket piano” — best PianoCraft-specific follow-up.** Start with 8–12 transverse modes and one or two voices, a precomputed velocity-dependent hammer/excitation pulse, and at most four shared body modes. Omit the iterative contact solver, coupled unisons, phantom/longitudinal modes, and sympathetic bank. A later version can add a second normal mode for only the strongest few partials.
3. **Flute — good monophonic experiment.** A single breath/excitation path, small filter chain, and short wind waveguide should be a contained fixed-point port. Use block-rate coefficients and avoid a full multi-instrument framework.
4. **Oboe/clarinet, then horn — plausible but riskier.** Reduced mono reed/lip waveguides are small in memory, but nonlinear stability, aliasing, and any oversampling requirement need measurement before promising real-time operation.
5. **Bowed strings — weakest first port.** The sustained bow contact and desired polyphony make it less attractive than the struck models. Try one voice without a body only after the simpler engines establish a fixed-point and profiling workflow.

## 5. Concrete next experiment

**Build a three-voice, 12-mode harp voice as a standalone `fm1-base` app, with no radio and one CPU core.**

1. Copy the [`fm1-base` template](https://github.com/charlesvestal/fm1-base/blob/02f422c7d9de121ca099bb2fdebe5c035f59e8ad/template/app.h) into a separate experiment. Replace its sine voice in `app_render`; retain its MIDI, keys, MASTER, packaging, and host simulator. Disable BLE for the first memory/CPU baseline.
2. Offline-convert one calibrated C4 harp note into 12 dominant mode frequency/decay/gain triplets and a short derived excitation. Store static coefficients only—no recording playback. Map K1 to decay and K2 to brightness, preserving 7-bit input at the control boundary.
3. Implement three statically allocated voices using single-precision `float` first, with coefficients evaluated at note-on/block boundaries. Compare a host render against a desktop render of the *same reduced model*. If the device misses budget, convert the mode update to fixed point using Felucca's [`phys_dsp.c`](https://github.com/hugelton/Felucca/blob/406b328ee43ffb8fd3d2eb4cce1a741db4a4cea1/firmware/src/phys_dsp.c) as the target-specific reference; do not start with a desktop solver.
4. On hardware, play repeated three-note chords for ten minutes while logging worst-block time with `plat_ticks24`, plus `plat_cpu_pct` and `plat_xruns` from [`plat.h`](https://github.com/charlesvestal/fm1-base/blob/02f422c7d9de121ca099bb2fdebe5c035f59e8ad/include/plat.h). The proposed gate is **zero underruns, no numerical instability, and worst sustained render load below 75%**, leaving time for panel/MIDI work. Listen for decay, brightness, and voice-stealing artifacts against the reduced desktop reference.
5. If it misses the gate, reduce to two voices/eight modes and compare fixed point before considering the second core. If it passes, add **one** feature at a time: four shared body modes or three sympathetic strings, then more notes. Use the resulting measurements to size the pocket-piano experiment rather than extrapolating from desktop performance.

The deliverable of that experiment is not a finished instrument; it is a measured cycles/memory/polyphony envelope for PianoCraft's modal arithmetic on the FM-1.
