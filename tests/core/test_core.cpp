// Core sequencer tests for M0. Self-contained: a tiny runner, no framework.
//
// Each TEST registers a function. main() runs them all, prints failures, and
// returns non-zero if any check failed, so ctest picks it up.

#include "phylo/Scene.h"
#include "phylo/Sequencer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
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
// Scheduled pattern changes (M1)

namespace
{
bool hasErrorAt(const phylo::ParseResult& r, int line)
{
    for (const auto& e : r.errors)
        if (e.line == line)
            return true;
    return false;
}

std::vector<Absolute> sortedRender(int blockSize, std::int64_t frames, bool scheduleAt50k)
{
    phylo::Pattern a(4.0);
    a.addNote(0.0, 60, 100, 0.5);
    phylo::Pattern b(4.0);
    b.addNote(0.0, 62, 100, 0.5);

    auto s = makeSequencer(120.0, 48000.0, a);
    s.play();
    std::vector<Absolute> all = render(s, 50000, blockSize);
    if (scheduleAt50k)
        s.scheduleNextBar(b);
    auto rest = render(s, frames - 50000, blockSize);
    all.insert(all.end(), rest.begin(), rest.end());
    std::sort(all.begin(), all.end());
    return all;
}
} // namespace

TEST(meter_sets_bar_length)
{
    phylo::Sequencer s;
    s.setTempo(120.0);
    s.setSampleRate(48000.0);
    s.setMeter(3);
    CHECK_EQ(s.meter(), 3);
    CHECK(std::abs(s.framesPerBar() - 72000.0) < 1e-9);
}

TEST(scheduled_pattern_starts_on_next_bar)
{
    // Bar = 4 beats = 96000 frames at 120 bpm, 48 kHz.
    phylo::Pattern a(4.0);
    a.addNote(0.0, 60, 100, 0.5);
    phylo::Pattern b(4.0);
    b.addNote(0.0, 62, 100, 0.5);

    auto s = makeSequencer(120.0, 48000.0, a);
    s.play();
    render(s, 50000, 512); // mid first bar

    s.scheduleNextBar(b);
    CHECK(s.hasPendingPattern());
    CHECK_EQ(s.pendingSwitchFrame(), 96000);

    const auto msgs = render(s, 150000, 512); // 50000 .. 200000

    std::int64_t firstNew = -1;
    bool oldAfterSwitch = false;
    for (const auto& m : msgs)
    {
        if (m.note == 62 && m.status == 0x90 && firstNew < 0)
            firstNew = m.frame;
        if (m.note == 60 && m.status == 0x90 && m.frame >= 96000)
            oldAfterSwitch = true;
    }
    CHECK_EQ(firstNew, 96000);
    CHECK(!oldAfterSwitch);
    CHECK(!s.hasPendingPattern());
}

TEST(scheduled_pattern_at_exact_bar_moves_to_following_bar)
{
    phylo::Pattern a(4.0);
    a.addNote(0.0, 60, 100, 0.5);
    phylo::Pattern b(4.0);
    b.addNote(0.0, 62, 100, 0.5);

    auto s = makeSequencer(120.0, 48000.0, a);
    s.play();
    render(s, 96000, 512); // playhead now sits on a bar line

    s.scheduleNextBar(b);
    CHECK_EQ(s.pendingSwitchFrame(), 192000);
}

TEST(scheduled_change_is_block_size_independent)
{
    const auto reference = sortedRender(4096, 300000, true);
    CHECK(!reference.empty());
    for (int block : {1, 7, 512, 150000})
    {
        const auto got = sortedRender(block, 300000, true);
        if (!(got == reference))
            std::printf("  FAIL [%s] block size %d differs from reference\n", gCurrent, block);
        CHECK(got == reference);
    }
}

TEST(rewind_drops_scheduled_change)
{
    phylo::Pattern a(4.0);
    a.addNote(0.0, 60, 100, 0.5);
    auto s = makeSequencer(120.0, 48000.0, a);
    s.play();
    render(s, 1000, 256);
    s.scheduleNextBar(phylo::Pattern(4.0));
    s.rewind();
    CHECK(!s.hasPendingPattern());
}

TEST(empty_pattern_still_advances_the_clock)
{
    phylo::Pattern empty(4.0);
    auto s = makeSequencer(120.0, 48000.0, empty);
    s.play();
    render(s, 1000, 256);
    CHECK_EQ(s.positionFrames(), 1000);
}

// ---------------------------------------------------------------------------
// Scene language (M1)

TEST(scene_parses_a_valid_file)
{
    const std::string text =
        "tempo 96\n"
        "meter 3\n"
        "key F# minor\n"
        "play bass\n"
        "// a comment line\n"
        "\n"
        "pattern bass 4\n"
        "  0    C2 100 0.5   // trailing comment\n"
        "  2.5  Bb2 90 1\n"
        "pattern lead 2\n"
        "  0 64 80 0.25\n"
        "section intro 8\n"
        "  play bass\n";
    const auto r = phylo::parseScene(text);
    CHECK(r.ok());
    CHECK(std::abs(r.scene.tempo - 96.0) < 1e-9);
    CHECK_EQ(r.scene.meter, 3);
    CHECK(r.scene.hasKey);
    CHECK(r.scene.keyRoot == "F#");
    CHECK(r.scene.keyMinor);
    CHECK(r.scene.activePattern == "bass");
    CHECK_EQ(r.scene.patterns.size(), 2u);
    CHECK_EQ(r.scene.sections.size(), 1u);
    if (r.scene.patterns.size() == 2 && r.scene.patterns[0].notes.size() == 2)
    {
        CHECK_EQ(r.scene.patterns[0].notes[0].note, 36); // C2
        CHECK_EQ(r.scene.patterns[0].notes[1].note, 46); // Bb2
        CHECK(std::abs(r.scene.patterns[0].notes[1].beat - 2.5) < 1e-9);
        CHECK_EQ(r.scene.patterns[1].notes[0].note, 64);
    }
    if (r.scene.sections.size() == 1)
    {
        CHECK_EQ(r.scene.sections[0].bars, 8);
        CHECK_EQ(r.scene.sections[0].play.size(), 1u);
    }
}

TEST(scene_empty_or_comment_only_is_valid)
{
    const auto r = phylo::parseScene("\n\n// nothing here\n");
    CHECK(r.ok());
    CHECK_EQ(r.scene.patterns.size(), 0u);
}

TEST(note_names_map_to_midi_numbers)
{
    struct Case { const char* token; int midi; };
    const Case valid[] = {
        {"C4", 60}, {"A4", 69}, {"C-1", 0}, {"G9", 127}, {"Bb-1", 10},
        {"F#3", 54}, {"Cb4", 59}, {"64", 64}, {"0", 0}, {"127", 127},
    };
    for (const auto& c : valid)
    {
        int m = -1;
        const bool okParse = phylo::parseNoteToken(c.token, m);
        if (!okParse || m != c.midi)
            std::printf("  FAIL [%s] %s -> %d (ok=%d), want %d\n", gCurrent, c.token, m, okParse ? 1 : 0, c.midi);
        CHECK(okParse && m == c.midi);
    }

    const char* invalid[] = {"H4", "C10", "C#-2", "128", "", "C", "c4", "C4x"};
    for (const char* t : invalid)
    {
        int m = 0;
        if (phylo::parseNoteToken(t, m))
            std::printf("  FAIL [%s] %s should be invalid\n", gCurrent, t);
        CHECK(!phylo::parseNoteToken(t, m));
    }
}

TEST(parse_errors_report_line_numbers)
{
    const std::string text =
        "tempo 500\n"           // 1: out of range
        "meter 4\n"             // 2
        "pattern bass 4\n"      // 3
        "  0 C2 100 0.5\n"      // 4
        "  4 C2 100 0.5\n"      // 5: beat outside pattern
        "  1 C2 0 0.5\n"        // 6: velocity 0
        "  2 H2 100 0.5\n"      // 7: not a note
        "wobble\n"              // 8: unknown directive
        "  stray\n";            // 9: indented outside any block
    const auto r = phylo::parseScene(text);
    CHECK(!r.ok());
    for (int line : {1, 5, 6, 7, 8, 9})
    {
        if (!hasErrorAt(r, line))
            std::printf("  FAIL [%s] expected an error on line %d\n", gCurrent, line);
        CHECK(hasErrorAt(r, line));
    }
    CHECK(!hasErrorAt(r, 2));
    CHECK(!hasErrorAt(r, 4));
}

TEST(unknown_pattern_references_are_reported)
{
    const std::string text =
        "pattern a 4\n"        // 1
        "  0 C2 100 1\n"       // 2
        "section s 2\n"        // 3
        "  play nope\n"        // 4
        "play missing\n";      // 5
    const auto r = phylo::parseScene(text);
    CHECK(!r.ok());
    CHECK(hasErrorAt(r, 4));
    CHECK(hasErrorAt(r, 5));
}

TEST(duplicate_pattern_names_are_reported)
{
    const auto r = phylo::parseScene("pattern a 4\npattern a 2\n");
    CHECK(!r.ok());
    CHECK(hasErrorAt(r, 2));
}

TEST(same_pattern_compares_content)
{
    const auto a = phylo::parseScene("pattern p 4\n  0 C4 100 0.5\n");
    const auto b = phylo::parseScene("pattern q 4\n  0 C4 100 0.5\n");
    const auto c = phylo::parseScene("pattern p 4\n  0 C4 100 0.6\n");
    CHECK(a.ok() && b.ok() && c.ok());
    if (a.ok() && b.ok() && c.ok())
    {
        CHECK(phylo::samePattern(a.scene.patterns[0], b.scene.patterns[0]) == true);
        CHECK(phylo::samePattern(a.scene.patterns[0], c.scene.patterns[0]) == false);
    }
}

TEST(parsed_pattern_plays_through_the_sequencer)
{
    const auto r = phylo::parseScene("pattern p 4\n  0 C4 100 0.5\n");
    CHECK(r.ok());
    if (r.ok() && r.scene.patterns.size() == 1)
    {
        auto s = makeSequencer(120.0, 48000.0, phylo::buildPattern(r.scene.patterns[0]));
        s.play();
        const auto msgs = render(s, 96000, 1024);
        CHECK_EQ(msgs.size(), 2u);
        if (msgs.size() == 2)
        {
            CHECK_EQ(msgs[0].note, 60);
            CHECK_EQ(msgs[1].frame, 12000);
        }
    }
}

// ---------------------------------------------------------------------------
// Regressions from review of M1

TEST(switch_releases_a_note_the_old_pattern_leaves_sounding)
{
    // Note C4 starts on beat 3 and is meant to end on beat 4, the bar line. The
    // switch drops the old note-off, so the sequencer must release the note itself.
    phylo::Pattern a(4.0);
    a.addNote(3.0, 60, 100, 1.0);
    phylo::Pattern b(4.0);
    b.addNote(0.0, 62, 100, 0.5);

    auto s = makeSequencer(120.0, 48000.0, a);
    s.play();
    auto msgs = render(s, 80000, 512); // past the on at 72000
    s.scheduleNextBar(b);
    const auto rest = render(s, 40000, 512); // 80000 .. 120000
    msgs.insert(msgs.end(), rest.begin(), rest.end());

    int onCount = 0, offCount = 0;
    std::int64_t offFrame = -1;
    for (const auto& m : msgs)
    {
        if (m.note != 60)
            continue;
        if (m.status == 0x90)
            ++onCount;
        if (m.status == 0x80)
        {
            ++offCount;
            offFrame = m.frame;
        }
    }
    CHECK_EQ(onCount, 1);
    CHECK_EQ(offCount, 1);
    CHECK_EQ(offFrame, 96000);
}

TEST(switch_at_a_fractional_bar_does_not_duplicate_the_bar_line_event)
{
    // At 130 bpm a bar is 88615.38 frames. The old pattern's beat 0 falls at frame
    // 88615, which is the bar line. The new pattern must start there, once.
    phylo::Pattern a(4.0);
    a.addNote(0.0, 60, 100, 0.5);
    phylo::Pattern b(4.0);
    b.addNote(0.0, 72, 100, 0.5);

    auto s = makeSequencer(130.0, 48000.0, a);
    s.play();
    render(s, 50000, 512);
    s.scheduleNextBar(b);
    const auto msgs = render(s, 200000, 512);

    int onAtBarLine = 0;
    bool oldAfterBar = false;
    for (const auto& m : msgs)
    {
        if (m.status == 0x90 && m.frame == 88615)
            ++onAtBarLine;
        if (m.status == 0x90 && m.note == 60 && m.frame >= 88615)
            oldAfterBar = true;
    }
    CHECK_EQ(onAtBarLine, 1);
    CHECK(!oldAfterBar);
}

TEST(tempo_change_keeps_the_playhead_on_its_beat)
{
    phylo::Pattern p(4.0);
    p.addNote(0.0, 60, 100, 0.5);
    p.addNote(3.0, 65, 100, 0.5);
    auto s = makeSequencer(120.0, 48000.0, p);
    s.play();
    render(s, 72000, 512); // playhead on beat 3.0

    s.setTempo(60.0); // one beat is now 48000 frames
    CHECK(std::abs(s.positionBeats() - 3.0) < 1e-9);

    const auto msgs = render(s, 60000, 512); // 72000 .. 132000
    std::int64_t beat3 = -1, beat4 = -1;
    for (const auto& m : msgs)
    {
        if (m.status == 0x90 && m.note == 65 && beat3 < 0)
            beat3 = m.frame;
        if (m.status == 0x90 && m.note == 60 && beat4 < 0)
            beat4 = m.frame;
    }
    CHECK_EQ(beat3, 72000);
    CHECK_EQ(beat4, 120000);
}

TEST(invalid_tempo_is_ignored_by_the_sequencer)
{
    phylo::Sequencer s;
    s.setTempo(120.0);
    s.setTempo(std::nan(""));
    s.setTempo(-5.0);
    s.setTempo(0.0);
    CHECK(std::abs(s.tempo() - 120.0) < 1e-9);
    s.setSampleRate(std::numeric_limits<double>::infinity());
    CHECK(std::abs(s.getSampleRate() - 48000.0) < 1e-9);
}

TEST(scene_rejects_non_finite_and_malformed_numbers)
{
    const char* bad[] = {
        "tempo nan\n",
        "tempo inf\n",
        "tempo 0x40\n",
        "meter 1e300\n",
        "pattern p nan\n",
        "pattern p 4\n  0 C4 100 nan\n",
        "pattern p 4\n  nan C4 100 1\n",
        "key 1 major\n",
        "key C0 minor\n",
    };
    for (const char* text : bad)
    {
        const auto r = phylo::parseScene(text);
        if (r.ok())
            std::printf("  FAIL [%s] accepted: %s", gCurrent, text);
        CHECK(!r.ok());
    }
}

TEST(scene_accepts_cr_only_line_endings)
{
    const auto r = phylo::parseScene("tempo 100\rmeter 3\rpattern p 4\r  0 C4 100 0.5\r");
    CHECK(r.ok());
    if (r.ok())
    {
        CHECK(std::abs(r.scene.tempo - 100.0) < 1e-9);
        CHECK_EQ(r.scene.patterns[0].notes.size(), 1u);
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
