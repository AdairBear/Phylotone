// Core sequencer tests for M0. Self-contained: a tiny runner, no framework.
//
// Each TEST registers a function. main() runs them all, prints failures, and
// returns non-zero if any check failed, so ctest picks it up.

#include "phylo/Sequencer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace
{

struct TestCase
{
    const char* name;
    std::function<void()> body;
};

std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

int gFailures = 0;
const char* gCurrent = "";

struct Registrar
{
    Registrar(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

#define TEST(name)                                                  \
    static void name();                                             \
    static Registrar name##_registrar(#name, &name);                \
    static void name()

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::printf("  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, gCurrent, #cond); \
            ++gFailures;                                                              \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        const auto va = (a);                                                                        \
        const auto vb = (b);                                                                        \
        if (!(va == vb)) {                                                                          \
            std::printf("  FAIL %s:%d [%s] %s == %s (got %lld, want %lld)\n", __FILE__, __LINE__,   \
                        gCurrent, #a, #b, static_cast<long long>(va), static_cast<long long>(vb));  \
            ++gFailures;                                                                            \
        }                                                                                           \
    } while (0)

// One emitted MIDI message placed on the absolute timeline.
struct Absolute
{
    std::int64_t frame;
    std::uint8_t status;
    std::uint8_t note;
    std::uint8_t velocity;

    bool operator<(const Absolute& o) const
    {
        return std::tie(frame, status, note, velocity) < std::tie(o.frame, o.status, o.note, o.velocity);
    }
    bool operator==(const Absolute& o) const
    {
        return frame == o.frame && status == o.status && note == o.note && velocity == o.velocity;
    }
};

// Runs the sequencer for `totalFrames` in blocks of `blockSize` and returns every
// message placed on the absolute timeline, in emission order.
std::vector<Absolute> render(phylo::Sequencer& seq, std::int64_t totalFrames, int blockSize)
{
    std::vector<Absolute> out;
    std::vector<phylo::MidiOut> block;
    std::int64_t done = 0;
    while (done < totalFrames)
    {
        const int n = static_cast<int>(std::min<std::int64_t>(blockSize, totalFrames - done));
        const std::int64_t start = seq.positionFrames();
        block.clear();
        seq.process(n, block);
        for (const auto& m : block)
        {
            // The offset must sit inside the block that produced it.
            CHECK(m.sampleOffset >= 0 && m.sampleOffset < n);
            out.push_back({start + m.sampleOffset, m.status, m.data1, m.data2});
        }
        done += n;
    }
    return out;
}

phylo::Sequencer makeSequencer(double bpm, double sr, const phylo::Pattern& p)
{
    phylo::Sequencer s;
    s.setTempo(bpm);
    s.setSampleRate(sr);
    s.setPattern(p);
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Timing

TEST(frames_per_beat_at_120_bpm_48k)
{
    phylo::Sequencer s;
    s.setTempo(120.0);
    s.setSampleRate(48000.0);
    CHECK(std::abs(s.framesPerBeat() - 24000.0) < 1e-9);
}

TEST(frames_per_beat_follows_tempo_and_rate)
{
    phylo::Sequencer s;
    s.setTempo(90.0);
    s.setSampleRate(44100.0);
    CHECK(std::abs(s.framesPerBeat() - 29400.0) < 1e-9);
    CHECK(std::abs(s.positionBeats()) < 1e-12);
}

// ---------------------------------------------------------------------------
// Single note

TEST(single_note_on_at_start_off_after_one_beat)
{
    phylo::Pattern p(4.0);
    p.addNote(0.0, 60, 100, 1.0);
    auto s = makeSequencer(120.0, 48000.0, p);
    s.play();

    const auto msgs = render(s, 48000, 512);
    CHECK_EQ(msgs.size(), 2u);
    if (msgs.size() == 2)
    {
        CHECK_EQ(msgs[0].frame, 0);
        CHECK_EQ(msgs[0].status, 0x90);
        CHECK_EQ(msgs[0].note, 60);
        CHECK_EQ(msgs[0].velocity, 100);
        CHECK_EQ(msgs[1].frame, 24000);
        CHECK_EQ(msgs[1].status, 0x80);
        CHECK_EQ(msgs[1].note, 60);
    }
}

TEST(note_off_clamped_to_pattern_end)
{
    phylo::Pattern p(2.0);
    p.addNote(1.5, 64, 90, 4.0); // would run past the loop end
    const auto& ev = p.events();
    CHECK_EQ(ev.size(), 2u);
    if (ev.size() == 2)
    {
        CHECK(std::abs(ev[1].beat - 2.0) < 1e-12);
        CHECK(!ev[1].on);
    }
}

TEST(notes_outside_the_loop_are_ignored)
{
    phylo::Pattern p(4.0);
    p.addNote(-0.5, 60, 100, 1.0);
    p.addNote(4.0, 60, 100, 1.0);
    p.addNote(9.0, 60, 100, 1.0);
    CHECK_EQ(p.events().size(), 0u);
}

// ---------------------------------------------------------------------------
// Loop wrap

TEST(loop_wraps_and_repeats_every_pattern_length)
{
    // 4 beats at 120 bpm, 48 kHz: the loop is 96000 frames.
    phylo::Pattern p(4.0);
    p.addNote(3.5, 48, 80, 0.25);
    auto s = makeSequencer(120.0, 48000.0, p);
    s.play();

    const auto msgs = render(s, 96000 * 3, 1024);

    // Expect one on and one off per loop, three loops.
    CHECK_EQ(msgs.size(), 6u);
    const std::int64_t onAt3_5 = 84000;   // 3.5 beats
    const std::int64_t offAt3_75 = 90000; // 3.75 beats
    for (int loop = 0; loop < 3; ++loop)
    {
        const std::int64_t base = loop * 96000LL;
        if (msgs.size() >= static_cast<std::size_t>(2 * loop + 2))
        {
            CHECK_EQ(msgs[2 * loop].frame, base + onAt3_5);
            CHECK_EQ(msgs[2 * loop].status, 0x90);
            CHECK_EQ(msgs[2 * loop + 1].frame, base + offAt3_75);
            CHECK_EQ(msgs[2 * loop + 1].status, 0x80);
        }
    }
}

TEST(loop_boundary_event_lands_in_the_next_loop)
{
    // A note at beat 0 of loop 2 sits exactly on a loop boundary frame.
    phylo::Pattern p(4.0);
    p.addNote(0.0, 60, 100, 0.5);
    auto s = makeSequencer(120.0, 48000.0, p);
    s.play();

    const auto msgs = render(s, 96000 * 2, 96000 * 2);
    std::vector<std::int64_t> onFrames;
    for (const auto& m : msgs)
        if (m.status == 0x90)
            onFrames.push_back(m.frame);
    CHECK_EQ(onFrames.size(), 2u);
    if (onFrames.size() == 2)
    {
        CHECK_EQ(onFrames[0], 0);
        CHECK_EQ(onFrames[1], 96000);
    }
}

// ---------------------------------------------------------------------------
// Block-size independence

TEST(output_is_identical_for_any_block_size)
{
    phylo::Pattern p(4.0);
    p.addNote(0.0, 60, 100, 1.0);
    p.addNote(0.5, 64, 90, 0.5);
    p.addNote(1.0, 67, 80, 1.0);
    p.addNote(2.25, 72, 70, 0.75);
    p.addNote(3.9, 48, 110, 0.4); // wraps past the loop end, clamped

    std::vector<Absolute> reference;
    {
        auto s = makeSequencer(133.0, 48000.0, p);
        s.play();
        reference = render(s, 96000 * 4, 1);
    }
    CHECK(!reference.empty());

    for (int block : {7, 64, 255, 512, 1024, 4096, 96000})
    {
        auto s = makeSequencer(133.0, 48000.0, p);
        s.play();
        auto got = render(s, 96000 * 4, block);
        // Compare as sorted sets: the frame and content must match exactly.
        auto a = reference;
        std::sort(a.begin(), a.end());
        std::sort(got.begin(), got.end());
        const bool same = a == got;
        if (!same)
            std::printf("  FAIL [%s] block size %d differs (%zu vs %zu messages)\n", gCurrent, block,
                        a.size(), got.size());
        CHECK(same);
    }
}

TEST(each_event_emitted_exactly_once_across_blocks)
{
    phylo::Pattern p(1.0);
    p.addNote(0.25, 60, 100, 0.5);
    auto s = makeSequencer(120.0, 48000.0, p);
    s.play();
    // 6 loops of 24000 frames, rendered in odd-sized blocks.
    const auto msgs = render(s, 24000 * 6, 333);
    CHECK_EQ(msgs.size(), 12u);
}

// ---------------------------------------------------------------------------
// Stop and play

TEST(stop_silences_output_and_freezes_playhead)
{
    phylo::Pattern p(4.0);
    p.addNote(0.0, 60, 100, 1.0);
    auto s = makeSequencer(120.0, 48000.0, p);

    // Stopped from the start: nothing emitted, playhead does not move.
    const auto silent = render(s, 48000, 512);
    CHECK(silent.empty());
    CHECK_EQ(s.positionFrames(), 0);

    s.play();
    render(s, 1000, 512);
    CHECK_EQ(s.positionFrames(), 1000);

    s.stop();
    const auto stopped = render(s, 48000, 512);
    CHECK(stopped.empty());
    CHECK_EQ(s.positionFrames(), 1000);
}

TEST(play_after_stop_resumes_from_position)
{
    phylo::Pattern p(4.0);
    p.addNote(1.0, 60, 100, 0.5); // on at frame 24000
    auto s = makeSequencer(120.0, 48000.0, p);

    s.play();
    render(s, 10000, 256); // position 10000
    s.stop();
    render(s, 5000, 256);  // silent, still at 10000
    s.play();

    const auto msgs = render(s, 30000, 256); // covers 10000 .. 40000
    CHECK_EQ(msgs.size(), 2u);
    if (msgs.size() == 2)
    {
        CHECK_EQ(msgs[0].frame, 24000);
        CHECK_EQ(msgs[0].status, 0x90);
        CHECK_EQ(msgs[1].frame, 36000);
        CHECK_EQ(msgs[1].status, 0x80);
    }
}

TEST(rewind_resets_playhead_to_start)
{
    phylo::Pattern p(4.0);
    p.addNote(0.0, 60, 100, 1.0);
    auto s = makeSequencer(120.0, 48000.0, p);
    s.play();
    render(s, 50000, 512);
    s.rewind();
    CHECK_EQ(s.positionFrames(), 0);
    const auto msgs = render(s, 1024, 1024);
    CHECK(!msgs.empty());
    if (!msgs.empty())
        CHECK_EQ(msgs[0].frame, 0);
}

// ---------------------------------------------------------------------------
// Retrigger ordering

TEST(retrigger_puts_note_off_before_note_on_at_same_frame)
{
    // Same note ends at beat 1 and restarts at beat 1.
    phylo::Pattern p(4.0);
    p.addNote(0.0, 60, 100, 1.0);
    p.addNote(1.0, 60, 90, 1.0);

    const auto& ev = p.events();
    CHECK_EQ(ev.size(), 4u);
    if (ev.size() == 4)
    {
        // Sorted: on@0, off@1, on@1, off@2. Off must precede the on at beat 1.
        CHECK(ev[1].beat == 1.0 && !ev[1].on);
        CHECK(ev[2].beat == 1.0 && ev[2].on);
    }

    auto s = makeSequencer(120.0, 48000.0, p);
    s.play();
    const auto msgs = render(s, 96000, 4096);
    CHECK_EQ(msgs.size(), 4u);
    if (msgs.size() == 4)
    {
        CHECK_EQ(msgs[1].frame, 24000);
        CHECK_EQ(msgs[1].status, 0x80);
        CHECK_EQ(msgs[2].frame, 24000);
        CHECK_EQ(msgs[2].status, 0x90);
        CHECK_EQ(msgs[2].velocity, 90);
    }
}

TEST(retrigger_ordering_holds_inside_one_block)
{
    // The off and on land in the same block. Order in the output must still be off, on.
    phylo::Pattern p(4.0);
    p.addNote(0.0, 60, 100, 1.0);
    p.addNote(1.0, 60, 90, 1.0);
    auto s = makeSequencer(120.0, 48000.0, p);
    s.play();

    std::vector<phylo::MidiOut> block;
    s.process(96000, block); // one large block covering both events
    std::vector<std::uint8_t> statuses;
    for (const auto& m : block)
        if (m.sampleOffset == 24000)
            statuses.push_back(m.status);
    CHECK_EQ(statuses.size(), 2u);
    if (statuses.size() == 2)
    {
        CHECK_EQ(statuses[0], 0x80);
        CHECK_EQ(statuses[1], 0x90);
    }
}

// ---------------------------------------------------------------------------
// Runner

int main()
{
    int ran = 0;
    for (const auto& t : registry())
    {
        gCurrent = t.name;
        const int before = gFailures;
        t.body();
        ++ran;
        std::printf("%s %s\n", gFailures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("\n%d tests, %d failed checks\n", ran, gFailures);
    return gFailures == 0 ? 0 : 1;
}
