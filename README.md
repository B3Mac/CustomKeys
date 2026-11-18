# CustomKeys

A low-latency macOS keyboard instrument that plays your own sample libraries.
It runs as a standalone app first, with AU, VST3 and AAX builds coming from the same code.

**Status:** plays Nord Piano Library pianos (`.npno`) with their velocity layers,
sustain loops and the sustain pedal. With nothing loaded, each note plays a test tone
so the MIDI-to-audio path and latency can be checked.

## Playing your Nord pianos

Click **Load piano...** (or drop a `.npno` file on the window) and pick a piano from
your Nord Piano Library folder. CustomKeys decodes it into memory on your Mac, which
takes a few seconds for a large piano, and reopens it the next time you start the app.
Nothing is copied or converted: the file is read where it is, so its drive must be
connected.

What's in so far: every velocity layer, picked the way the Nord picks it; sustain
loops with their decay; the sustain pedal; a short key-up fade. Not yet: release
sounds, pedal noise, string resonance, stretch tuning and per-piano damper times.

The `.npno` reader is a C++ port of [OpenNord](https://github.com/simonflore/opennord)'s
and is licensed AGPL-3.0; see [engine/src/nord/NOTICE.md](engine/src/nord/NOTICE.md).
Nord audio never goes into this repository.

## Layout

| Folder | What lives there |
| --- | --- |
| `engine/` | The sound engine: plain C++20, no JUCE, no UI. Real-time safe: no allocation, locks or disk access on the audio thread. Instrument loaders (`engine/src/nord/`) run on background threads. |
| `app/` | The JUCE wrapper and UI. One plugin target builds the standalone app, AU, VST3 and (with the SDK) AAX. |
| `tests/` | Engine tests, including a check that the audio path never allocates. |
| `cmake/` | Pinned JUCE download. |

## Building

You need CMake 3.22+ and Xcode (macOS) or a C++20 compiler (engine only, any OS).

```sh
# Engine and tests only (fast, any OS)
cmake -S . -B build -DCUSTOMKEYS_BUILD_APP=OFF
cmake --build build
ctest --test-dir build --output-on-failure

# Full macOS app and plugins (downloads JUCE the first time)
cmake -S . -B build-mac -G Xcode
cmake --build build-mac --config Release --target CustomKeys_Standalone CustomKeys_AU CustomKeys_VST3
open build-mac/app/CustomKeys_artefacts/Release/Standalone/CustomKeys.app
```

In the standalone app, choose your audio interface and buffer size under
**Options > Audio/MIDI Settings**. The header shows the buffer latency, DSP load and voice count.
64 or 128 samples is the target.

### AAX

AAX builds need Avid's AAX SDK and PACE signing, which can't be committed here.
Once you have the SDK:

```sh
cmake -S . -B build-mac -G Xcode -DCUSTOMKEYS_AAX_SDK_PATH=/path/to/aax-sdk
```

## Continuous integration

Every push and pull request runs the engine tests on macOS and Linux, and builds the
macOS Standalone, AU and VST3. The unsigned build is attached to each run as
`CustomKeys-macOS-unsigned`. Because it is unsigned, right-click the app and choose **Open** the first time.

## Licence

CustomKeys is built on [JUCE](https://juce.com), used under the JUCE licence.
The Nord Piano reader in `engine/src/nord/` is a port of OpenNord's and is AGPL-3.0.
CustomKeys is for personal use; a build that included it could only be shared under the AGPL.
Nord is a trademark of Clavia DMI AB; CustomKeys is not affiliated with Clavia.
