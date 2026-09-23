// Offline chord-recognition evaluator: runs the same ChordEngine as the plugin
// over a WAV file, prints the recognised segments as a .lab file (Harte
// syntax, as used by MIREX/mir_eval), and optionally scores them against a
// reference .lab.
//
//   Tools/eval/build.sh
//   Tools/eval/build/notecap_eval take1.wav                  # print segments
//   Tools/eval/build/notecap_eval take1.wav take1.lab        # + accuracy
//   options: --gate -50  --sensitivity 50  --no7  --sus  --dimaug  --out est.lab
#include "core/ChordEngine.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace notecap;

namespace
{
    // ---- minimal WAV reader: PCM 16/24/32-bit int or 32-bit float, any channel count ----
    bool readWav (const char* path, std::vector<float>& mono, double& rate)
    {
        std::ifstream f (path, std::ios::binary);
        if (! f) return false;
        auto u32 = [&] { uint32_t v = 0; f.read ((char*) &v, 4); return v; };
        auto u16 = [&] { uint16_t v = 0; f.read ((char*) &v, 2); return v; };

        char id[4];
        f.read (id, 4); u32(); char wave[4]; f.read (wave, 4);
        if (std::strncmp (id, "RIFF", 4) != 0 || std::strncmp (wave, "WAVE", 4) != 0) return false;

        uint16_t format = 0, channels = 0, bits = 0;
        while (f.read (id, 4))
        {
            const uint32_t size = u32();
            if (std::strncmp (id, "fmt ", 4) == 0)
            {
                format = u16(); channels = u16(); rate = u32(); u32(); u16(); bits = u16();
                f.seekg (size - 16, std::ios::cur);
                if (format == 0xfffe) format = bits == 32 ? 3 : 1;  // WAVE_FORMAT_EXTENSIBLE: assume PCM/float by depth
            }
            else if (std::strncmp (id, "data", 4) == 0)
            {
                if (channels == 0) return false;
                std::vector<uint8_t> raw (size);
                f.read ((char*) raw.data(), size);
                const int bytes = bits / 8;
                const size_t frames = size / (size_t) (bytes * channels);
                mono.assign (frames, 0.0f);
                for (size_t i = 0; i < frames; ++i)
                    for (int c = 0; c < channels; ++c)
                    {
                        const uint8_t* p = raw.data() + (i * channels + (size_t) c) * (size_t) bytes;
                        float v = 0.0f;
                        if (format == 3 && bits == 32) std::memcpy (&v, p, 4);
                        else if (bits == 16) v = (int16_t) (p[0] | (p[1] << 8)) / 32768.0f;
                        else if (bits == 24) v = (int32_t) ((p[0] << 8) | (p[1] << 16) | (p[2] << 24)) / 2147483648.0f;
                        else if (bits == 32) { int32_t s; std::memcpy (&s, p, 4); v = s / 2147483648.0f; }
                        mono[i] += v / channels;
                    }
                return true;
            }
            else
                f.seekg (size + (size & 1), std::ios::cur);
        }
        return false;
    }

    std::string harte (const Chord& c)
    {
        if (c.isNone()) return "N";
        static const char* q[] = { "maj", "min", "7", "maj7", "min7", "dim", "aug", "sus2", "sus4", "hdim7" };
        std::string s = std::string (pitchClassName (c.root)) + ":" + q[(int) c.quality];
        if (c.bass >= 0)
        {
            static const char* deg[] = { "1", "b2", "2", "b3", "3", "4", "b5", "5", "b6", "6", "b7", "7" };
            s += "/"; s += deg[(c.bass - c.root + 12) % 12];
        }
        return s;
    }

    struct Seg { double start, end; std::string label; };

    // "maj" / "min" / "N", or "X" when the label is outside the majmin vocabulary.
    std::string majMinOf (const std::string& label)
    {
        if (label == "N" || label.empty()) return "N";
        const auto colon = label.find (':');
        const std::string rootStr = label.substr (0, colon);
        static const char* names[][2] = { { "C", "B#" }, { "C#", "Db" }, { "D", "D" }, { "D#", "Eb" }, { "E", "Fb" }, { "F", "E#" },
                                          { "F#", "Gb" }, { "G", "G" }, { "G#", "Ab" }, { "A", "A" }, { "A#", "Bb" }, { "B", "Cb" } };
        int root = -1;
        for (int i = 0; i < 12; ++i)
            if (rootStr == names[i][0] || rootStr == names[i][1]) root = i;
        if (root < 0) return "X";
        std::string q = colon == std::string::npos ? "maj" : label.substr (colon + 1);
        q = q.substr (0, q.find ('/'));
        const bool minor = q.rfind ("min", 0) == 0;
        const bool major = q.empty() || q == "maj" || q == "7" || q == "maj7" || q.rfind ("maj", 0) == 0 || q == "9";
        if (! minor && ! major) return "X";
        return std::to_string (root) + (minor ? ":min" : ":maj");
    }

    std::vector<Seg> readLab (const char* path)
    {
        std::vector<Seg> out;
        std::ifstream f (path);
        std::string line;
        while (std::getline (f, line))
        {
            std::istringstream ss (line);
            Seg s;
            if (ss >> s.start >> s.end >> s.label) out.push_back (s);
        }
        return out;
    }

    std::string labelAt (const std::vector<Seg>& segs, double t)
    {
        for (const auto& s : segs)
            if (t >= s.start && t < s.end) return s.label;
        return "N";
    }
}

int main (int argc, char** argv)
{
    const char* wavPath = nullptr;
    const char* refPath = nullptr;
    const char* outPath = nullptr;
    ChordEngine::Params p;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--gate" && i + 1 < argc) p.gateDb = std::stof (argv[++i]);
        else if (a == "--sensitivity" && i + 1 < argc) p.minScore = 0.75f - 0.3f * std::stof (argv[++i]) / 100.0f;
        else if (a == "--no7") p.sevenths = false;
        else if (a == "--sus") p.suspended = true;
        else if (a == "--dimaug") p.dimAug = true;
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (! wavPath) wavPath = argv[i];
        else if (! refPath) refPath = argv[i];
    }
    if (! wavPath)
    {
        std::fprintf (stderr, "usage: notecap_eval audio.wav [reference.lab] [--gate dB] [--sensitivity 0-100] [--no7] [--sus] [--dimaug] [--out est.lab]\n");
        return 2;
    }

    std::vector<float> audio;
    double fs = 0;
    if (! readWav (wavPath, audio, fs)) { std::fprintf (stderr, "could not read %s\n", wavPath); return 1; }

    ChordEngine engine;
    engine.prepare (fs);
    engine.setParams (p);

    struct Ev { uint32_t seg; double onset, detect; Chord chord; };
    std::vector<Ev> evs;
    ChordEventList list;
    double latencySum = 0; int latencyN = 0;
    for (size_t pos = 0; pos < audio.size(); pos += 512)
    {
        list.clear();
        engine.process (audio.data() + pos, (int) std::min<size_t> (512, audio.size() - pos), (int64_t) pos, list);
        for (const auto& e : list)
        {
            bool replaced = false;
            for (auto& x : evs)
                if (x.seg == e.segment) { x.chord = e.chord; replaced = true; }
            if (! replaced)
            {
                evs.push_back ({ e.segment, e.onsetSample / fs, e.detectSample / fs, e.chord });
                if (! e.chord.isNone()) { latencySum += (e.detectSample - e.onsetSample) / fs; ++latencyN; }
            }
        }
    }

    const double duration = audio.size() / fs;
    std::vector<Seg> est;
    double t = 0.0; std::string current = "N";
    for (const auto& e : evs)
    {
        const double start = std::max (t, e.onset);
        if (start > t) est.push_back ({ t, start, current });
        t = start; current = harte (e.chord);
    }
    est.push_back ({ t, duration, current });

    FILE* out = outPath ? std::fopen (outPath, "w") : stdout;
    for (const auto& s : est)
        if (s.end > s.start) std::fprintf (out, "%.3f\t%.3f\t%s\n", s.start, s.end, s.label.c_str());
    if (outPath) std::fclose (out);

    std::fprintf (stderr, "%s: %.1f s, %.0f Hz, %zu chord events, mean decision latency %.0f ms, tuning %+.0f cents\n",
                  wavPath, duration, fs, evs.size(), latencyN ? 1000.0 * latencySum / latencyN : 0.0, engine.getTuningCents());

    if (refPath)
    {
        const auto ref = readLab (refPath);
        int scored = 0, correct = 0;
        for (double x = 0.0; x < duration; x += 0.01)   // 10 ms sampling, as in MIREX
        {
            const auto r = majMinOf (labelAt (ref, x));
            if (r == "X") continue;
            ++scored;
            if (majMinOf (labelAt (est, x)) == r) ++correct;
        }
        std::fprintf (stderr, "majmin weighted accuracy: %.1f%% (%d of %d 10 ms frames)\n",
                      scored ? 100.0 * correct / scored : 0.0, correct, scored);
    }
    return 0;
}
