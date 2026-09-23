// Real-time grid quantisation. A real-time quantiser can only delay events, never
// move them earlier, so "Nearest" snaps to the grid line nearest the *onset* when
// that line is still ahead of us, and otherwise fires immediately flagged as late
// (the MIDI port back-dates late events to the grid line they belong to).
#pragma once

#include <cmath>
#include <cstdint>

namespace notecap
{
    enum class QuantizeMode { Off, Nearest, Next };

    struct QuantizeGrid
    {
        double stepPpq = 0.5;  // grid step in quarter notes; <= 0 means no grid
        double swing = 0.0;    // 0..1: delays every odd grid line by swing * step / 2

        bool active() const noexcept { return stepPpq > 0.0; }

        double line (int64_t k) const noexcept
        {
            const double base = (double) k * stepPpq;
            return (k & 1) ? base + swing * 0.5 * stepPpq : base;
        }

        // First grid line at or after p.
        double nextLine (double p) const noexcept
        {
            const int64_t k0 = (int64_t) std::floor (p / stepPpq) - 1;
            for (int64_t k = k0; k < k0 + 4; ++k)
                if (line (k) >= p - 1.0e-9) return line (k);
            return line (k0 + 4);
        }

        double nearestLine (double p) const noexcept
        {
            const int64_t k0 = (int64_t) std::floor (p / stepPpq);
            double best = line (k0 - 1);
            for (int64_t k = k0; k <= k0 + 2; ++k)
                if (std::abs (line (k) - p) < std::abs (best - p)) best = line (k);
            return best;
        }
    };

    struct QuantizeDecision
    {
        double fireAtPpq;   // when the event should be sent (>= now)
        double stampPpq;    // where the event belongs musically (may be < now when late)
        bool late;
    };

    inline QuantizeDecision quantize (double onsetPpq, double nowPpq, QuantizeMode mode,
                                      const QuantizeGrid& grid) noexcept
    {
        if (mode == QuantizeMode::Off || ! grid.active())
            return { nowPpq, nowPpq, false };

        if (mode == QuantizeMode::Next)
        {
            const double t = grid.nextLine (nowPpq);
            return { t, t, false };
        }

        const double t = grid.nearestLine (onsetPpq);
        if (t >= nowPpq)
            return { t, t, false };
        return { nowPpq, t, true };
    }

    // Grid step choices exposed in the UI, in quarter notes.
    inline double gridStepForIndex (int index) noexcept
    {
        static const double steps[] = { 0.0, 4.0, 2.0, 1.0, 0.5, 0.25, 2.0 / 3.0, 1.0 / 3.0, 1.0 / 6.0 };
        return (index >= 0 && index < (int) (sizeof (steps) / sizeof (steps[0]))) ? steps[index] : 0.0;
    }
}
