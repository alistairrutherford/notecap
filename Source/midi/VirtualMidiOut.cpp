#include "VirtualMidiOut.h"

#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <mutex>
#include <set>

namespace
{
    // One CoreMIDI client per process, shared by every instance, plus the set of
    // port indices in use so each instance gets a distinct, stable port name.
    // The client is never disposed: Apple warns that disposing a process's last
    // client can make the MIDI server exit, after which MIDIClientCreate fails.
    struct Registry
    {
        std::mutex lock;
        MIDIClientRef client = 0;
        std::set<int> usedIndices;

        static Registry& get()
        {
            static Registry r;
            return r;
        }
    };

    const mach_timebase_info_data_t& timebase()
    {
        static const mach_timebase_info_data_t tb = [] { mach_timebase_info_data_t t; mach_timebase_info (&t); return t; }();
        return tb;
    }

    MIDITimeStamp nsToHostTicks (int64_t ns)
    {
        if (ns <= 0) return 0;
        const auto& tb = timebase();
        return (MIDITimeStamp) ((__int128) ns * tb.denom / tb.numer);
    }

    // Stable per-name unique ID so Live remembers the port's Track/Remote settings.
    SInt32 uniqueIdForIndex (int index)
    {
        return (SInt32) (0x4E430000 | (index & 0xffff)); // 'NC' + index
    }
}

VirtualMidiOut::VirtualMidiOut() : juce::Thread ("NoteCap MIDI out") {}

VirtualMidiOut::~VirtualMidiOut()
{
    close();
}

int64_t VirtualMidiOut::nowNs() noexcept
{
    const auto& tb = timebase();
    return (int64_t) ((__int128) mach_absolute_time() * tb.numer / tb.denom);
}

bool VirtualMidiOut::open (int preferredIndex)
{
    close();

    auto& reg = Registry::get();
    std::lock_guard<std::mutex> guard (reg.lock);

    if (reg.client == 0)
    {
        const OSStatus err = MIDIClientCreateWithBlock (CFSTR ("NoteCap"), &reg.client, nullptr);
        if (err != noErr)
        {
            reg.client = 0;
            lastError.store ((int32_t) err);
            return false;
        }
    }

    int chosen = preferredIndex >= 1 && reg.usedIndices.count (preferredIndex) == 0 ? preferredIndex : 0;
    for (int i = 1; chosen == 0; ++i)
        if (reg.usedIndices.count (i) == 0) chosen = i;

    MIDIEndpointRef endpoint = 0;
    const CFStringRef name = nameForIndex (chosen).toCFString();
    const OSStatus err = MIDISourceCreateWithProtocol (reg.client, name, kMIDIProtocol_1_0, &endpoint);
    CFRelease (name);
    if (err != noErr || endpoint == 0)
    {
        lastError.store (err != noErr ? (int32_t) err : -1);
        return false;
    }

    MIDIObjectSetIntegerProperty (endpoint, kMIDIPropertyUniqueID, uniqueIdForIndex (chosen)); // may fail if taken; harmless
    MIDIObjectSetStringProperty (endpoint, kMIDIPropertyManufacturer, CFSTR ("NoteCap"));

    reg.usedIndices.insert (chosen);
    lastError.store (0);
    index = chosen;
    activeNotes = {};
    latestStamp = 0;
    source.store ((uint32_t) endpoint);
    startThread (juce::Thread::Priority::highest);
    return true;
}

void VirtualMidiOut::close()
{
    const auto endpoint = (MIDIEndpointRef) source.load();
    if (endpoint == 0)
        return;

    // Let the sender flush anything already queued (e.g. final note-offs).
    signalThreadShouldExit();
    stopThread (500);
    drain();
    releaseActiveNotes();

    source.store (0);
    MIDIEndpointDispose (endpoint);

    auto& reg = Registry::get();
    std::lock_guard<std::mutex> guard (reg.lock);
    reg.usedIndices.erase (index);
    index = 0;
}

bool VirtualMidiOut::push (const uint8_t* bytes, int numBytes, int64_t hostTimeNs) noexcept
{
    if (source.load (std::memory_order_relaxed) == 0 || numBytes < 1 || numBytes > 3)
        return false;

    const auto scope = fifo.write (1);
    if (scope.blockSize1 + scope.blockSize2 < 1)
        return false;

    auto& m = buffer[scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2];
    m.hostTimeNs = hostTimeNs;
    m.size = (uint8_t) numBytes;
    for (int i = 0; i < 3; ++i) m.bytes[i] = i < numBytes ? bytes[i] : 0;
    return true;
}

void VirtualMidiOut::run()
{
    while (! threadShouldExit())
    {
        drain();
        wait (1);
    }
}

void VirtualMidiOut::drain()
{
    const auto endpoint = (MIDIEndpointRef) source.load();
    if (endpoint == 0)
        return;

    // Each message becomes one packet with its own timestamp.
    alignas (MIDIEventList) uint8_t storage[4096];
    auto* list = reinterpret_cast<MIDIEventList*> (storage);

    for (;;)
    {
        const int ready = fifo.getNumReady();
        if (ready == 0)
            return;

        MIDIEventPacket* packet = MIDIEventListInit (list, kMIDIProtocol_1_0);
        int taken = 0;
        const auto scope = fifo.read (std::min (ready, 64));
        auto addRange = [&] (int start, int size)
        {
            for (int i = start; i < start + size && packet != nullptr; ++i)
            {
                const auto& m = buffer[i];
                const int status = m.bytes[0] & 0xf0, channel = m.bytes[0] & 0x0f, note = m.bytes[1] & 0x7f;
                if (status == 0x90 && m.bytes[2] > 0) activeNotes[(size_t) channel].set ((size_t) note);
                else if (status == 0x80 || status == 0x90) activeNotes[(size_t) channel].reset ((size_t) note);
                // Universal MIDI Packet, MIDI 1.0 channel voice message (type 2), group 0.
                const UInt32 word = (0x2u << 28) | ((UInt32) m.bytes[0] << 16) | ((UInt32) m.bytes[1] << 8) | (UInt32) m.bytes[2];
                const MIDITimeStamp stamp = nsToHostTicks (m.hostTimeNs);
                latestStamp = std::max<MIDITimeStampValue> (latestStamp, stamp);
                packet = MIDIEventListAdd (list, sizeof (storage), packet, stamp, 1, &word);
                ++taken;
            }
        };
        addRange (scope.startIndex1, scope.blockSize1);
        addRange (scope.startIndex2, scope.blockSize2);

        if (taken > 0 && MIDIReceivedEventList (endpoint, list) == noErr)
            numSent.fetch_add ((uint32_t) taken);
    }
}

void VirtualMidiOut::releaseActiveNotes()
{
    const auto endpoint = (MIDIEndpointRef) source.load();
    if (endpoint == 0)
        return;

    // CoreMIDI holds future-stamped events until their time and drops whatever is
    // still pending when the source is disposed. So: release immediately (covers
    // notes already delivered), release again after the latest scheduled event
    // (covers note-ons still pending), and give the latter a moment to go out.
    const MIDITimeStamp now = mach_absolute_time();
    const MIDITimeStamp after = std::max<MIDITimeStampValue> (now, latestStamp + 1);

    auto sendReleases = [&] (MIDITimeStamp when)
    {
        alignas (MIDIEventList) uint8_t storage[4096];
        auto* list = reinterpret_cast<MIDIEventList*> (storage);
        MIDIEventPacket* packet = MIDIEventListInit (list, kMIDIProtocol_1_0);
        int count = 0;
        for (UInt32 ch = 0; ch < 16; ++ch)
            for (UInt32 note = 0; note < 128 && packet != nullptr; ++note)
                if (activeNotes[ch].test (note))
                {
                    const UInt32 word = (0x2u << 28) | ((0x80u | ch) << 16) | (note << 8);
                    packet = MIDIEventListAdd (list, sizeof (storage), packet, when, 1, &word);
                    ++count;
                }
        if (count > 0)
            MIDIReceivedEventList (endpoint, list);
        return count;
    };

    if (sendReleases (now) > 0 && after > now)
    {
        sendReleases (after);
        const int64_t waitNs = std::min<int64_t> (250'000'000, (int64_t) ((__int128) (after - now) * timebase().numer / timebase().denom));
        juce::Thread::sleep ((int) (waitNs / 1'000'000) + 2);
    }
    activeNotes = {};
}
