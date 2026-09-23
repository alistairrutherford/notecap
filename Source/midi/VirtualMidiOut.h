// A CoreMIDI virtual source owned by one plugin instance ("NoteCap Out <n>").
//
// Ableton Live can't route MIDI out of AU plugins, but it sees virtual CoreMIDI
// sources like hardware ports, so a MIDI track can take this port as its input
// and record it. The audio thread pushes time-stamped messages into a lock-free
// FIFO; a sender thread hands them to CoreMIDI.
#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>

using MIDITimeStampValue = uint64_t;

class VirtualMidiOut : private juce::Thread
{
public:
    VirtualMidiOut();
    ~VirtualMidiOut() override;

    // Opens the port with the lowest free index >= 1, preferring `preferredIndex`.
    // Message thread only. Returns false if CoreMIDI refused.
    bool open (int preferredIndex = 0);
    void close();

    bool isOpen() const noexcept { return source.load() != 0; }
    int getIndex() const noexcept { return index; }
    juce::String getName() const { return nameForIndex (index); }

    // The CoreMIDI endpoint (MIDIEndpointRef), 0 when closed. Lets in-process
    // clients (tests) connect to this exact port rather than resolving it by name.
    uint32_t getEndpoint() const noexcept { return source.load(); }

    static juce::String nameForIndex (int i) { return "NoteCap Out " + juce::String (i); }

    // Audio thread (real-time safe). `hostTimeNs` uses the mach host clock.
    // Returns false if the FIFO is full or the port is closed.
    bool push (const uint8_t* bytes, int numBytes, int64_t hostTimeNs) noexcept;

    // Current mach host time in ns, for callers without a host-supplied timestamp.
    static int64_t nowNs() noexcept;

    uint32_t getNumSent() const noexcept { return numSent.load(); }

private:
    void run() override;
    void drain();
    void releaseActiveNotes();

    struct Message
    {
        int64_t hostTimeNs;
        uint8_t bytes[3];
        uint8_t size;
    };

    static constexpr int fifoSize = 1024;
    juce::AbstractFifo fifo { fifoSize };
    Message buffer[fifoSize];

    std::atomic<uint32_t> source { 0 };   // MIDIEndpointRef
    int index = 0;
    std::atomic<uint32_t> numSent { 0 };

    // Notes the receiver currently holds, per channel. Only touched by the
    // consumer side (sender thread, or close() once that thread has stopped),
    // so closing the port can always release them: no stuck notes.
    std::array<std::bitset<128>, 16> activeNotes {};
    MIDITimeStampValue latestStamp = 0;   // CoreMIDI delivers at the timestamp, so releases must not precede it

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VirtualMidiOut)
};
