// MPE into a firmware that listens on one channel: the notes of every member channel go to that
// channel, and the expression (pitch bend, pressure, CC74) of the note played last and still held
// is the channel's. Lower zone: channel 1 the master, 2..16 the members.
#pragma once

#include <array>
#include <cstdint>
#include <algorithm>

class MpeFold
{
public:
    /** Bend ranges in semitones: the MPE controller's per-note and master ranges (MPE's defaults),
        and the firmware's own (a synth's usual 2). */
    static constexpr int perNoteRange = 48, masterRange = 2, firmwareRange = 2;

    void reset()
    {
        held.fill (0);
        stack = {};
        depth = 0;
        bend.fill (8192);
        pressure.fill (0);
        timbre.fill (64);
        masterBend = 8192;
        lastBend = -1;
    }

    /** One message in (len 2 or 3), what to send to the firmware's channel `out` (0..15) out
        through `emit (status, d1, d2, len)`. */
    template <typename Emit>
    void process (uint8_t status, uint8_t d1, uint8_t d2, int len, int out, Emit&& emit)
    {
        const int type = status & 0xf0, ch = status & 0x0f;
        const auto to = [out] (int t) { return (uint8_t) (t | out); };

        switch (type)
        {
            case 0x90:
                if (d2 > 0)
                {
                    push (ch, d1);
                    sendExpression (out, emit, true);
                    if (held[d1]++ > 0)
                        emit (to (0x80), d1, 0, 3); // (the same pitch on two channels: one voice)
                    emit (to (0x90), d1, d2, 3);
                    return;
                }
                [[fallthrough]];
            case 0x80:
            {
                const int was = focus();
                pop (ch, d1);
                if (held[d1] > 0 && --held[d1] == 0)
                    emit (to (0x80), d1, type == 0x80 ? d2 : 0, 3);
                if (focus() != was && focus() >= 0)
                    sendExpression (out, emit, true);
                return;
            }
            case 0xe0:
            {
                const int v = d1 | (d2 << 7);
                if (ch == 0) masterBend = v; else bend[(size_t) ch] = v;
                if (ch == 0 || ch == focus())
                    sendExpression (out, emit, false);
                return;
            }
            case 0xd0:
                pressure[(size_t) ch] = d1;
                if (ch == 0 || ch == focus())
                    emit (to (0xd0), d1, 0, 2);
                return;
            case 0xa0: // poly pressure: as the note's own pressure
                if (ch == focus() || ch == 0)
                    emit (to (0xd0), d2, 0, 2);
                return;
            case 0xb0:
                if (d1 == 74 && ch != 0)
                {
                    timbre[(size_t) ch] = d2;
                    if (ch == focus())
                        emit (to (0xb0), 74, d2, 3);
                    return;
                }
                if (d1 == 123 || d1 == 120) // all notes / sound off: forget what is held
                {
                    if (ch == 0)
                    {
                        held.fill (0);
                        depth = 0;
                    }
                }
                emit (to (0xb0), d1, d2, 3);
                return;
            default:
                emit (to (type), d1, d2, len);
                return;
        }
    }

private:
    int focus() const { return depth > 0 ? stack[(size_t) depth - 1].ch : -1; }

    void push (int ch, int note)
    {
        pop (ch, note);
        if (depth == (int) stack.size())
        {
            std::move (stack.begin() + 1, stack.end(), stack.begin());
            --depth;
        }
        stack[(size_t) depth++] = { (uint8_t) ch, (uint8_t) note };
    }
    void pop (int ch, int note)
    {
        for (int i = depth - 1; i >= 0; --i)
            if (stack[(size_t) i].ch == ch && stack[(size_t) i].note == note)
            {
                std::move (stack.begin() + i + 1, stack.begin() + depth, stack.begin() + i);
                --depth;
                return;
            }
    }

    template <typename Emit>
    void sendExpression (int out, Emit&& emit, bool all)
    {
        const int f = focus();
        const double semis = (masterBend - 8192) / 8192.0 * masterRange
                           + (f > 0 ? (bend[(size_t) f] - 8192) / 8192.0 * perNoteRange : 0.0);
        const int v = std::clamp ((int) (8192.0 + semis / firmwareRange * 8192.0 + 0.5), 0, 16383);
        if (v != lastBend)
        {
            emit ((uint8_t) (0xe0 | out), (uint8_t) (v & 0x7f), (uint8_t) (v >> 7), 3);
            lastBend = v;
        }
        if (all && f > 0)
        {
            emit ((uint8_t) (0xd0 | out), (uint8_t) pressure[(size_t) f], 0, 2);
            emit ((uint8_t) (0xb0 | out), 74, (uint8_t) timbre[(size_t) f], 3);
        }
    }

    struct Held { uint8_t ch, note; };
    std::array<int, 128> held {};
    std::array<Held, 64> stack {};
    int depth = 0;
    std::array<int, 16> bend {}, pressure {}, timbre {};
    int masterBend = 8192, lastBend = -1;

public:
    MpeFold() { reset(); }
};
