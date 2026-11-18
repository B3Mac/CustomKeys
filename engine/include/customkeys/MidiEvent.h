#pragma once

#include <cstdint>

namespace ck {

/** A short MIDI message placed at a sample position inside the current audio block.
    The host or app layer copies raw bytes in; the engine interprets them. */
struct MidiEvent
{
    uint32_t sampleOffset = 0; ///< Position in the block, 0 .. numSamples - 1.
    uint8_t status = 0;        ///< Status byte, e.g. 0x90 for note-on on channel 1.
    uint8_t data1 = 0;
    uint8_t data2 = 0;

    static constexpr MidiEvent noteOn (uint32_t offset, uint8_t note, uint8_t velocity) { return { offset, 0x90, note, velocity }; }
    static constexpr MidiEvent noteOff (uint32_t offset, uint8_t note)                  { return { offset, 0x80, note, 0 }; }
    static constexpr MidiEvent controller (uint32_t offset, uint8_t cc, uint8_t value)   { return { offset, 0xB0, cc, value }; }
    static constexpr MidiEvent sustain (uint32_t offset, bool down)                      { return controller (offset, 64, down ? 127 : 0); }
};

} // namespace ck
