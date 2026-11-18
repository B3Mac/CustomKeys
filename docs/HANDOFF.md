# CustomKeys handoff

Everything another developer or agent needs to pick up CustomKeys. Last updated 2026-11-18.

## Goal

A low-latency macOS keyboard instrument, in the spirit of Keyscape, that plays sample
libraries Brendan provides. Nord Piano Library pianos are the main target; open formats
(SFZ with WAV/FLAC first) come next.

## Decisions so far

| Decision | Detail |
| --- | --- |
| Personal use only | CustomKeys is not for sale or distribution. |
| Standalone first | AU, VST3 and AAX build from the same code later, with no rewrite. |
| Framework | JUCE 8 (pinned in `cmake/JUCE.cmake`), C++20, CMake. |
| Engine is JUCE-free | `engine/` is plain C++ so it can be tested anywhere and reused by every plugin format. |
| Nord `.npno` files | Read through a C++ port of [OpenNord](https://github.com/simonflore/opennord)'s open decoder (PR #2). Files are decoded in memory at load time, on Brendan's Mac. |
| No Nord audio in git | No `.npno`, `.nsmp`, decoded WAVs or bulk Nord data in the repo, tests, releases or PRs. Tests use synthetic files. |
| Licence | The Nord reader is AGPL-3.0 (from OpenNord). Fine for personal use; any shared build would have to be AGPL. |
| No decompiling commercial apps | MainStage and Keyscape were studied for ideas only. No code, samples or artwork from them, and no cracking Spectrasonics libraries. |
| Preferred open format | SFZ with WAV or FLAC samples. Recorded instruments are saved as SFZ + FLAC. |

## Where things stand

| Item | State |
| --- | --- |
| [PR #1](https://github.com/B3Mac/CustomKeys/pull/1): skeleton | CI green, ready to merge. Engine, JUCE app, 14 engine tests, CI. Each key plays a test tone. |
| [PR #2](https://github.com/B3Mac/CustomKeys/pull/2): Nord pianos | Stacked on PR #1, CI green. Plays `.npno` pianos with velocity layers, sustain loops and the pedal. Adds the `Instrument` model and sample-playback voices. Being tested on Brendan's Mac Studio. |
| [Technical plan](https://claude.ai/artifact/VWzEoceUykzXqqo3pBZThi) | Framework choice, engine design, formats, UI mapping, milestones. |
| [MainStage and Keyscape study](https://claude.ai/artifact/DenR19MXhVsCZjB21HCVuh) | Learning notes and 7 recommendations, summarised below. |

Brendan's Nord libraries live on his Mac at `/Volumes/Lightbringer/Decomps/CustomKeys/Piano Libraries/`.
He owns a Nord. The pianos he uses most are Astoria Grand XL, Pearl Upright XL and White Grand XL.

## Build and run

```sh
# Engine and tests (any OS, no JUCE)
cmake -S . -B build -DCUSTOMKEYS_BUILD_APP=OFF
cmake --build build
ctest --test-dir build --output-on-failure

# macOS app and plugins (downloads JUCE the first time)
cmake -S . -B build-mac -G Xcode
cmake --build build-mac --config Release --target CustomKeys_Standalone CustomKeys_AU CustomKeys_VST3
open build-mac/app/CustomKeys_artefacts/Release/Standalone/CustomKeys.app
```

With PR #2, `build/customkeys_npno_check <file.npno>` loads a piano the way the app does,
which is a quick way to check a library without the UI.

Set the audio device and buffer under **Options > Audio/MIDI Settings** in the standalone app.
Aim for 64 samples. The header shows buffer latency, DSP load and voice count.

CI (`.github/workflows/ci.yml`) runs the engine tests on macOS and Linux and builds the
macOS Standalone, AU and VST3. It uploads them as an unsigned zip. GitHub no longer offers `macos-14`, so use
`macos-latest`.

## How the code is laid out

| Path | Role |
| --- | --- |
| `engine/include/customkeys/Engine.h` | Engine API: `prepare`, `process` (real-time), `setInstrument` (message thread), stats. |
| `engine/include/customkeys/Instrument.h` | What every loader builds: samples, zones (root key + 128-entry velocity table), key map, velocity gains. |
| `engine/src/Voice.cpp` | One sounding note: sample playback, loops, envelope, steal and damp fades. |
| `engine/src/nord/` | `.npno` reader (AGPL, see its `NOTICE.md`). |
| `app/Source/` | JUCE processor and editor. The same processor is used by every plugin format. |
| `tests/` | Engine tests with a tiny built-in harness and an allocation counter. |

Rules that keep latency low: `process()` never allocates, locks, frees memory or touches
the disk. Instruments are built on a background thread and swapped in atomically. A test
fails if the audio path allocates.

## What's next

In order:

1. **Merge PR #1, then PR #2** once Brendan has played the Nord pianos on his Mac.
2. **Open-format importers** (next PR, stacked on PR #2): SFZ with WAV/AIFF/FLAC, a folder
   mapper that reads note and velocity from file names, then SF2 and DecentSampler (`.dspreset`).
   All of them build the same `Instrument`.
3. **Nord realism:** release sounds, pedal noise, string resonance, stretch tuning, per-piano damper times.
4. **Disk streaming:** keep the first ~64k frames of each sample in RAM, lock those pages, and stream the
   rest on a helper thread that joins the device's audio workgroup. This is needed for libraries larger than RAM.
5. **Auto-sampler** (fallback and for other instruments): play a keyboard or plugin over MIDI, record it,
   and save SFZ + FLAC. It records velocity layers, pedal-down and release passes.
6. **Plugin builds:** AU validated with `auval`, AAX once Brendan has Avid's SDK and PACE signing
   (`-DCUSTOMKEYS_AAX_SDK_PATH=...`), host state save/recall.
7. **Stage-panel UI** following Brendan's reference image, in CustomKeys' own style.

## Takeaways from the MainStage and Keyscape study

1. Keep JUCE's Core Audio device. Default to a 64-frame buffer, offer 32 and 128, and show the device's reported latency.
2. If voices ever render on more than one thread, every worker joins the device's audio workgroup.
3. No page faults in the callback: preload sample starts (about 64k frames per zone) and touch or lock those pages at load.
4. No locks or allocation on the audio thread: lock-free queues for MIDI, patch swaps and UI messages.
5. Optional one-file container for big libraries, mapped once and read ahead sequentially.
6. Piano realism: many velocity layers (denser at the loud end), separate pedal-up and pedal-down sets, release samples, pedal noise.
7. Hold a no-idle-sleep power assertion while playing live; remember devices by UID and handle hot-plug.

## Open questions for Brendan

- Are the organ and synth sections in the reference UI needed for v1, or is piano-first enough?
- Which audio interface does he play through, and at what buffer size?
- Does he want AAX soon enough to start Avid's developer sign-up now?
