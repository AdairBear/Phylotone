// Core sequencer tests for M0. Self-contained: a tiny runner, no framework.
//
// Each TEST registers a function. main() runs them all, prints failures, and
// returns non-zero if any check failed, so ctest picks it up.

#include "phylo/Generator.h"
#include "phylo/host/Wire.h"
#include "phylo/Scene.h"
#include "phylo/assistant/Assistant.h"
#include "phylo/assistant/Json.h"
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
// JSON reader (M2)

TEST(json_parses_nested_values_and_escapes)
{
    phylo::json::Value v;
    std::string err;
    const bool ok = phylo::json::parse(
        R"({"a":1.5,"b":"x\"y\n\u00e9","c":[true,false,null],"d":{"e":-2e1}})", v, err);
    CHECK(ok);
    CHECK(err.empty());
    if (ok)
    {
        CHECK(v.get("a") != nullptr && std::abs(v.get("a")->number - 1.5) < 1e-12);
        CHECK(v.get("b") != nullptr && v.get("b")->str == "x\"y\n\xC3\xA9");
        CHECK(v.get("c") != nullptr && v.get("c")->items.size() == 3u);
        CHECK(v.get("d") != nullptr && v.get("d")->get("e") != nullptr &&
              std::abs(v.get("d")->get("e")->number + 20.0) < 1e-12);
        CHECK(v.get("missing") == nullptr);
    }
}

TEST(json_rejects_malformed_input)
{
    const char* bad[] = {"", "{", "[1,]", "{\"a\" 1}", "\"unterminated", "{} trailing", "nan", "01x", "\"\\q\""};
    for (const char* text : bad)
    {
        phylo::json::Value v;
        std::string err;
        if (phylo::json::parse(text, v, err))
            std::printf("  FAIL [%s] accepted: %s\n", gCurrent, text);
        CHECK(!phylo::json::parse(text, v, err));
    }
}

TEST(json_quote_round_trips)
{
    const std::string raw = "line1\nquote\" back\\slash \x01 tab\t";
    phylo::json::Value v;
    std::string err;
    CHECK(phylo::json::parse("{\"s\":" + phylo::json::quote(raw) + "}", v, err));
    CHECK(v.get("s") != nullptr && v.get("s")->str == raw);
}

// ---------------------------------------------------------------------------
// Assistant (M2)

namespace
{
using namespace phylo::assistant;

const char* kScene =
    "tempo 120\n"
    "meter 4\n"
    "play bass\n"
    "macro density 0.5\n"
    "pattern bass 4\n"
    "  0 C2 100 0.5\n"
    "  2 G2 90 0.5\n"
    "pattern lead 4\n"
    "  0 C4 80 1\n";

ToolCall call(const std::string& name, const std::string& args, const std::string& id = "c1")
{
    ToolCall c;
    c.id = id;
    c.name = name;
    c.argsJson = args;
    return c;
}

// Returns scripted responses in order. Records how many requests it saw.
class FakeProvider : public ChatProvider
{
public:
    std::vector<ChatResponse> script;
    std::size_t calls = 0;
    std::string lastUserText; // the text of the last user message the provider was sent
    std::string name() const override { return "fake"; }
    ChatResponse complete(const ChatRequest& req) override
    {
        ++calls;
        for (const auto& m : req.messages)
            if (m.role == Role::User)
                lastUserText = m.text;
        if (script.empty())
        {
            ChatResponse r;
            r.error = "script exhausted";
            return r;
        }
        ChatResponse r = script.front();
        script.erase(script.begin());
        return r;
    }
};

ChatResponse textReply(const std::string& text)
{
    ChatResponse r;
    r.ok = true;
    r.text = text;
    return r;
}

ChatResponse toolReply(const ToolCall& c)
{
    ChatResponse r;
    r.ok = true;
    r.toolCalls.push_back(c);
    return r;
}
} // namespace

TEST(off_mode_reads_but_refuses_changes)
{
    ProjectState p{kScene, Mode::Off};
    ActionLog log;
    Assistant a(p, log);
    CHECK(a.invoke(call("read_project", "{}")).ok);
    CHECK(a.invoke(call("explain_scene", "{}")).ok);
    const auto r = a.invoke(call("set_macro", R"({"name":"density","value":0.9,"reason":"test"})"));
    CHECK(!r.ok && !r.applied && !r.proposed);
    CHECK(p.sceneText == kScene);
    CHECK(log.entries().empty());
}

TEST(ask_mode_proposes_then_approve_applies_and_logs)
{
    ProjectState p{kScene, Mode::Ask};
    ActionLog log;
    Assistant a(p, log);

    const auto r = a.invoke(call("set_macro", R"({"name":"density","value":0.9,"reason":"more density"})"));
    CHECK(r.ok && r.proposed && !r.applied);
    CHECK(p.sceneText == kScene);
    CHECK_EQ(a.proposals().size(), 1u);

    const auto ap = a.approve(r.proposalId);
    CHECK(ap.ok && ap.applied);
    CHECK(p.sceneText.find("macro density 0.9") != std::string::npos);
    CHECK_EQ(log.entries().size(), 1u);
    if (!log.entries().empty())
    {
        CHECK(log.entries()[0].reason == "more density");
        CHECK(log.entries()[0].beforeText == kScene);
        CHECK(log.entries()[0].afterText == p.sceneText);
    }
    CHECK(a.proposals().empty());
}

TEST(assist_mode_applies_low_risk_and_holds_destructive)
{
    ProjectState p{kScene, Mode::Assist};
    ActionLog log;
    Assistant a(p, log);

    const auto low = a.invoke(call("set_macro", R"({"name":"density","value":0.2,"reason":"thinner"})"));
    CHECK(low.ok && low.applied);
    CHECK_EQ(log.entries().size(), 1u);

    // Removing the lead pattern is destructive: held for approval.
    const std::string noLead = "tempo 120\nmeter 4\nplay bass\nmacro density 0.2\npattern bass 4\n  0 C2 100 0.5\n  2 G2 90 0.5\n";
    const std::string args = R"({"scene":)" + phylo::json::quote(noLead) + R"(,"reason":"drop lead"})";
    const auto del = a.invoke(call("edit_scene", args));
    CHECK(del.ok && del.proposed && !del.applied);
    CHECK_EQ(log.entries().size(), 1u);

    // Adding a note is not destructive: applied.
    const std::string more = std::string(kScene) + "  3 D2 70 0.25\n";
    const auto add = a.invoke(call("edit_scene", R"({"scene":)" + phylo::json::quote(more) + R"(,"reason":"add a note"})"));
    CHECK(add.ok && add.applied);
}

TEST(edit_scene_rejects_text_that_does_not_parse)
{
    ProjectState p{kScene, Mode::Assist};
    ActionLog log;
    Assistant a(p, log);
    const std::string bad = "tempo nan\n";
    const auto r = a.invoke(call("edit_scene", R"({"scene":)" + phylo::json::quote(bad) + R"(,"reason":"x"})"));
    CHECK(!r.ok && !r.applied && !r.proposed);
    CHECK(p.sceneText == kScene);
    CHECK(log.entries().empty());
}

TEST(undo_reverts_in_order_and_stops_when_empty)
{
    ProjectState p{kScene, Mode::Assist};
    ActionLog log;
    Assistant a(p, log);
    a.invoke(call("set_macro", R"({"name":"density","value":0.1,"reason":"one"})"));
    a.invoke(call("set_macro", R"({"name":"density","value":0.2,"reason":"two"})"));
    CHECK_EQ(log.entries().size(), 2u);

    CHECK(a.invoke(call("undo", R"({"reason":"revert two"})")).applied);
    CHECK(p.sceneText.find("macro density 0.1") != std::string::npos);
    CHECK(a.invoke(call("undo", R"({"reason":"revert one"})")).applied);
    CHECK(p.sceneText == kScene);
    CHECK(!a.invoke(call("undo", R"({"reason":"nothing"})")).ok);
}

TEST(stale_proposal_is_refused_and_keeps_manual_edits)
{
    ProjectState p{kScene, Mode::Ask};
    ActionLog log;
    Assistant a(p, log);
    const auto r = a.invoke(call("set_macro", R"({"name":"density","value":0.9,"reason":"proposal"})"));
    CHECK(r.proposed);

    // The musician edits the scene by hand after the proposal was made.
    p.sceneText += "pattern extra 2\n  0 A3 60 0.5\n";
    const std::string manual = p.sceneText;

    const auto ap = a.approve(r.proposalId);
    CHECK(!ap.ok && !ap.applied);
    CHECK(p.sceneText == manual);
    CHECK(log.entries().empty());
}

TEST(explain_scene_describes_patterns_and_range)
{
    ProjectState p{kScene, Mode::Ask};
    ActionLog log;
    Assistant a(p, log);
    const auto r = a.invoke(call("explain_scene", "{}"));
    CHECK(r.ok);
    CHECK(r.resultJson.find("Tempo 120") != std::string::npos);
    CHECK(r.resultJson.find("bass") != std::string::npos);
    CHECK(r.resultJson.find("C2") != std::string::npos);
}

TEST(set_macro_adds_a_missing_macro_and_validates_range)
{
    ProjectState p{"tempo 120\npattern a 4\n  0 C4 100 1\n", Mode::Assist};
    ActionLog log;
    Assistant a(p, log);
    CHECK(a.invoke(call("set_macro", R"({"name":"space","value":0.25,"reason":"add"})")).applied);
    CHECK(p.sceneText.find("macro space 0.25") != std::string::npos);
    CHECK(!a.invoke(call("set_macro", R"({"name":"space","value":1.5,"reason":"bad"})")).ok);
}

TEST(changes_without_a_reason_are_refused)
{
    ProjectState p{kScene, Mode::Assist};
    ActionLog log;
    Assistant a(p, log);
    CHECK(!a.invoke(call("set_macro", R"({"name":"density","value":0.3})")).ok);
    CHECK(!a.invoke(call("set_macro", R"({"name":"density","value":0.3,"reason":""})")).ok);
    CHECK(log.entries().empty());
}

TEST(action_log_serialises_one_valid_json_object_per_line)
{
    ProjectState p{kScene, Mode::Assist};
    ActionLog log;
    Assistant a(p, log);
    a.invoke(call("set_macro", R"({"name":"density","value":0.3,"reason":"say \"hi\""})"));
    const std::string lines = log.toJsonLines();
    phylo::json::Value v;
    std::string err;
    CHECK(phylo::json::parse(lines.substr(0, lines.find('\n')), v, err));
    CHECK(v.get("reason") != nullptr && v.get("reason")->str == "say \"hi\"");
}

TEST(session_runs_a_tool_then_answers)
{
    ProjectState p{kScene, Mode::Assist};
    ActionLog log;
    Assistant a(p, log);
    FakeProvider fake;
    fake.script = {
        toolReply(call("set_macro", R"({"name":"density","value":0.7,"reason":"user asked"})", "t1")),
        textReply("Done: density is now 0.7."),
    };
    Session s(a, fake, "test-model");
    const auto out = s.send("make it denser");
    CHECK(out.ok);
    CHECK(out.text == "Done: density is now 0.7.");
    CHECK_EQ(out.toolCalls, 1);
    CHECK(p.sceneText.find("macro density 0.7") != std::string::npos);
    // user, assistant tool call, tool result, assistant text
    CHECK_EQ(s.history().size(), 4u);
    if (s.history().size() == 4)
        CHECK(s.history()[2].role == Role::Tool && s.history()[2].toolCallId == "t1");
}

TEST(session_sends_app_actions_with_the_next_user_message)
{
    ProjectState p{kScene, Mode::Off};
    ActionLog log;
    Assistant a(p, log);
    FakeProvider fake;
    fake.script = { textReply("ok"), textReply("ok again") };
    Session s(a, fake, "m");
    s.noteAppAction("The user rejected proposal 2.");
    s.noteAppAction("The user approved proposal 3.");
    CHECK(s.send("make it louder").ok);
    CHECK(fake.lastUserText.find("The user rejected proposal 2.") != std::string::npos);
    CHECK(fake.lastUserText.find("The user approved proposal 3.") != std::string::npos);
    CHECK(fake.lastUserText.find("make it louder") != std::string::npos);
    // Sent once: the next message carries no stale notes.
    CHECK(s.send("thanks").ok);
    CHECK(fake.lastUserText == "thanks");
}

TEST(session_stops_a_provider_that_loops_on_tools)
{
    ProjectState p{kScene, Mode::Off};
    ActionLog log;
    Assistant a(p, log);
    FakeProvider fake;
    for (int i = 0; i < 20; ++i)
        fake.script.push_back(toolReply(call("read_project", "{}", "r" + std::to_string(i))));
    Session s(a, fake, "m");
    const auto out = s.send("loop");
    CHECK(!out.ok);
    CHECK(out.error.find("stopped after") != std::string::npos);
    CHECK_EQ(fake.calls, static_cast<std::size_t>(Session::kMaxToolRounds + 1));
}

TEST(session_reports_provider_failure)
{
    ProjectState p{kScene, Mode::Ask};
    ActionLog log;
    Assistant a(p, log);
    FakeProvider fake;
    ChatResponse bad;
    bad.error = "HTTP 401: invalid key";
    fake.script = {bad};
    Session s(a, fake, "m");
    const auto out = s.send("hi");
    CHECK(!out.ok);
    CHECK(out.error == "HTTP 401: invalid key");
}

TEST(scene_macro_directive_parses_and_rejects_duplicates)
{
    const auto r = phylo::parseScene("macro density 0.5\nmacro density 0.6\nmacro big 2\n");
    CHECK(!r.ok());
    CHECK(hasErrorAt(r, 2));
    CHECK(hasErrorAt(r, 3));
    const auto good = phylo::parseScene("macro density 0.5\n");
    CHECK(good.ok());
    if (good.ok())
        CHECK(std::abs(good.scene.macros[0].value - 0.5) < 1e-12);
}


// ---------------------------------------------------------------------------
// Generator (M3)

namespace
{
const char* kGenScene =
    "tempo 120\n"
    "meter 4\n"
    "chords Am F C G\n"
    "seed 7\n"
    "generate pad bass\n"
    "macro density 0.6\n"
    "macro tension 0.7\n"
    "macro space 0.4\n";

phylo::ScenePattern takeFrom(const std::string& text)
{
    const auto r = phylo::parseScene(text);
    CHECK(r.ok());
    return phylo::generateTake(r.scene);
}

// Pitch classes of the chord for a bar, from the scene's own chord list.
// The pad test uses tension 0.7, so a flat seventh (10) may be added to any chord.
bool isChordTone(int note, const phylo::SceneChord& c)
{
    const int pc = note % 12;
    const int d = ((pc - c.rootPitchClass) % 12 + 12) % 12;
    if (d == 10)
        return true;
    switch (c.quality)
    {
    case phylo::ChordQuality::Minor:      return d == 0 || d == 3 || d == 7;
    case phylo::ChordQuality::Diminished: return d == 0 || d == 3 || d == 6;
    case phylo::ChordQuality::Dominant7:  return d == 0 || d == 4 || d == 7 || d == 10;
    case phylo::ChordQuality::Minor7:     return d == 0 || d == 3 || d == 7 || d == 10;
    case phylo::ChordQuality::Major7:     return d == 0 || d == 4 || d == 7 || d == 11;
    case phylo::ChordQuality::Major:
    default:                              return d == 0 || d == 4 || d == 7;
    }
}
} // namespace

TEST(generator_same_seed_and_settings_give_identical_notes)
{
    const auto a = takeFrom(kGenScene);
    const auto b = takeFrom(kGenScene);
    CHECK(!a.notes.empty());
    CHECK(phylo::samePattern(a, b));
    CHECK(a.notes.size() == b.notes.size());
}

TEST(generator_identical_midi_through_the_sequencer_twice)
{
    // The accept check of M3: the same seed and settings give the same MIDI output.
    auto render = [] {
        const auto r = phylo::parseScene(kGenScene);
        const phylo::Pattern p = phylo::buildPattern(phylo::generateTake(r.scene));
        phylo::Sequencer seq;
        seq.setSampleRate(48000.0);
        seq.setTempo(120.0);
        seq.setMeter(4);
        seq.setPattern(p);
        seq.play();
        std::vector<phylo::MidiOut> all;
        std::vector<phylo::MidiOut> block;
        for (int i = 0; i < 2000; ++i)
        {
            block.clear();
            seq.process(48, block);
            for (auto m : block)
            {
                m.sampleOffset += i * 48;
                all.push_back(m);
            }
        }
        return all;
    };
    const auto first = render();
    const auto second = render();
    CHECK(!first.empty());
    CHECK(first.size() == second.size());
    bool same = first.size() == second.size();
    for (std::size_t i = 0; same && i < first.size(); ++i)
        same = first[i].sampleOffset == second[i].sampleOffset && first[i].status == second[i].status &&
               first[i].data1 == second[i].data1 && first[i].data2 == second[i].data2;
    CHECK(same);
}

TEST(generator_different_seed_changes_the_bass_rolls)
{
    std::string other = kGenScene;
    other.replace(other.find("seed 7"), 6, "seed 8");
    const auto a = takeFrom(kGenScene);
    const auto b = takeFrom(other);
    CHECK(!phylo::samePattern(a, b));
}

TEST(generator_pads_use_the_chord_of_each_bar)
{
    const auto r = phylo::parseScene(kGenScene);
    CHECK(r.ok());
    const auto take = phylo::generateTake(r.scene);
    CHECK(std::abs(take.lengthBeats - 16.0) < 1e-12);

    for (const auto& n : take.notes)
    {
        const int bar = static_cast<int>(std::floor(n.beat / 4.0));
        const auto& chord = r.scene.chords[static_cast<std::size_t>(bar)];
        // Pads are the notes from 48 up; bass is from 36 up to 47.
        if (n.note >= 48)
            CHECK(isChordTone(n.note, chord));
        else
            CHECK(n.note % 12 == chord.rootPitchClass);
    }
}

TEST(generator_bass_plays_the_root_on_every_downbeat)
{
    const auto r = phylo::parseScene(kGenScene);
    const auto take = phylo::generateTake(r.scene);
    for (int bar = 0; bar < 4; ++bar)
    {
        bool found = false;
        for (const auto& n : take.notes)
            if (n.beat == bar * 4.0 && n.note < 48)
            {
                found = true;
                const auto& chord = r.scene.chords[static_cast<std::size_t>(bar)];
                CHECK(n.note == 36 + chord.rootPitchClass);
            }
        CHECK(found);
    }
}

TEST(generator_density_macro_only_adds_bass_notes)
{
    // Changing density must not move the pads or the downbeats, and more density adds notes.
    std::string sparse = kGenScene;
    sparse.replace(sparse.find("macro density 0.6"), 17, "macro density 0.0");
    std::string dense = kGenScene;
    dense.replace(dense.find("macro density 0.6"), 17, "macro density 1.0");
    const auto a = takeFrom(sparse);
    const auto b = takeFrom(dense);
    CHECK(a.notes.size() < b.notes.size());

    auto pads = [](const phylo::ScenePattern& p) {
        std::vector<phylo::SceneNote> out;
        for (const auto& n : p.notes)
            if (n.note >= 48)
                out.push_back(n);
        return out;
    };
    CHECK(pads(a).size() == pads(b).size());
    for (std::size_t i = 0; i < pads(a).size(); ++i)
        CHECK(pads(a)[i].note == pads(b)[i].note);
}

TEST(generator_tension_adds_a_seventh_to_triads)
{
    std::string low = kGenScene;
    low.replace(low.find("macro tension 0.7"), 17, "macro tension 0.1");
    const auto r = phylo::parseScene(low);
    const auto take = phylo::generateTake(r.scene);
    // With low tension, each bar has exactly three pad notes on its downbeat.
    int padsOnBar0 = 0;
    for (const auto& n : take.notes)
        if (n.beat == 0.0 && n.note >= 48)
            ++padsOnBar0;
    CHECK(padsOnBar0 == 3);

    const auto high = takeFrom(kGenScene);
    int highPads = 0;
    for (const auto& n : high.notes)
        if (n.beat == 0.0 && n.note >= 48)
            ++highPads;
    CHECK(highPads == 4);
}

TEST(scene_chords_seed_and_generate_parse)
{
    const auto r = phylo::parseScene(kGenScene);
    CHECK(r.ok());
    CHECK(r.scene.chords.size() == 4);
    CHECK(r.scene.chords[0].rootPitchClass == 9);
    CHECK(r.scene.chords[0].quality == phylo::ChordQuality::Minor);
    CHECK(r.scene.chords[1].rootPitchClass == 5);
    CHECK(r.scene.chords[1].quality == phylo::ChordQuality::Major);
    CHECK(r.scene.seed == 7);
    CHECK(r.scene.generate.size() == 2);
    CHECK(phylo::hasGeneratedTake(r.scene));
}

TEST(scene_chord_symbols_cover_the_qualities)
{
    const auto r = phylo::parseScene("chords C Cm C7 Cm7 Cmaj7 Cdim Bbm F#\n");
    CHECK(r.ok());
    CHECK(r.scene.chords.size() == 8);
    CHECK(r.scene.chords[2].quality == phylo::ChordQuality::Dominant7);
    CHECK(r.scene.chords[3].quality == phylo::ChordQuality::Minor7);
    CHECK(r.scene.chords[4].quality == phylo::ChordQuality::Major7);
    CHECK(r.scene.chords[5].quality == phylo::ChordQuality::Diminished);
    CHECK(r.scene.chords[6].rootPitchClass == 10);
    CHECK(r.scene.chords[7].rootPitchClass == 6);
}

TEST(scene_rejects_bad_chords_seed_and_generate)
{
    CHECK(!phylo::parseScene("chords C H\n").ok());
    CHECK(!phylo::parseScene("chords\n").ok());
    CHECK(!phylo::parseScene("chords C\nchords G\n").ok());
    CHECK(!phylo::parseScene("seed -1\n").ok());
    CHECK(!phylo::parseScene("seed 1.5\n").ok());
    CHECK(!phylo::parseScene("seed 4294967296\n").ok());
    CHECK(!phylo::parseScene("generate lead\nchords C\n").ok());
    CHECK(!phylo::parseScene("generate pad\n").ok()); // no chords line
    CHECK(!phylo::parseScene("chords C\ngenerate\n").ok());

    const auto r = phylo::parseScene("generate pad\n");
    CHECK(!r.ok());
    CHECK(r.errors.front().line == 1);
}

TEST(generator_without_a_generate_line_makes_no_take)
{
    const auto r = phylo::parseScene("chords C G\n");
    CHECK(r.ok());
    CHECK(!phylo::hasGeneratedTake(r.scene));
}


// ---------------------------------------------------------------------------
// Host wire protocol (M4)

namespace
{
std::vector<std::uint8_t> bytesOf(const std::string& s)
{
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

// Feeds bytes one at a time and collects every frame.
std::vector<phylo::host::Frame> decodeOneByteAtATime(const std::vector<std::uint8_t>& bytes, bool& failed)
{
    phylo::host::FrameDecoder dec;
    std::vector<phylo::host::Frame> out;
    for (std::uint8_t b : bytes)
    {
        dec.feed(&b, 1);
        phylo::host::Frame f;
        while (dec.next(f))
            out.push_back(f);
    }
    failed = dec.failed();
    return out;
}
} // namespace

TEST(wire_frames_survive_arbitrary_splits)
{
    auto a = phylo::host::encodeFrame(phylo::host::MsgType::Load, bytesOf("/plugins/Akazi.vst3"));
    auto b = phylo::host::encodeFrame(phylo::host::MsgType::Shutdown, {});
    std::vector<std::uint8_t> both = a;
    both.insert(both.end(), b.begin(), b.end());

    bool failed = true;
    const auto frames = decodeOneByteAtATime(both, failed);
    CHECK(!failed);
    CHECK(frames.size() == 2);
    if (frames.size() == 2)
    {
        CHECK(frames[0].type == phylo::host::MsgType::Load);
        CHECK(std::string(frames[0].payload.begin(), frames[0].payload.end()) == "/plugins/Akazi.vst3");
        CHECK(frames[1].type == phylo::host::MsgType::Shutdown);
        CHECK(frames[1].payload.empty());
    }
}

TEST(wire_truncated_frame_waits_and_does_not_fail)
{
    auto full = phylo::host::encodeFrame(phylo::host::MsgType::Error, bytesOf("boom"));
    full.pop_back();
    phylo::host::FrameDecoder dec;
    dec.feed(full.data(), full.size());
    phylo::host::Frame f;
    CHECK(!dec.next(f));
    CHECK(!dec.failed());
}

TEST(wire_oversize_length_is_refused_before_allocating)
{
    // Length field says 4 GB; the decoder must fail without waiting for the bytes.
    const std::vector<std::uint8_t> hostile = {0xFF, 0xFF, 0xFF, 0xFF, 0x05};
    phylo::host::FrameDecoder dec;
    dec.feed(hostile.data(), hostile.size());
    phylo::host::Frame f;
    CHECK(!dec.next(f));
    CHECK(dec.failed());
    CHECK(!dec.error().empty());
}

TEST(wire_unknown_type_fails_the_stream)
{
    const std::vector<std::uint8_t> bad = {0x00, 0x00, 0x00, 0x00, 0x99};
    phylo::host::FrameDecoder dec;
    dec.feed(bad.data(), bad.size());
    phylo::host::Frame f;
    CHECK(!dec.next(f));
    CHECK(dec.failed());
}

TEST(wire_process_round_trips_midi_and_planar_audio)
{
    phylo::host::ProcessRequest req;
    req.frames = 3;
    req.channels = 2;
    req.midi = {{0, 0x90, 60, 100}, {2, 0x80, 60, 0}};
    req.audio = {0.25f, -0.5f, 1.0f, -1.0f, 0.0f, 0.125f};

    phylo::host::ProcessRequest back;
    CHECK(phylo::host::decodeProcess(phylo::host::encodeProcess(req), back));
    CHECK(back.frames == 3 && back.channels == 2);
    CHECK(back.midi.size() == 2 && back.midi[1].frame == 2 && back.midi[1].status == 0x80);
    CHECK(back.audio.size() == 6);
    for (std::size_t i = 0; i < back.audio.size() && i < req.audio.size(); ++i)
        CHECK(back.audio[i] == req.audio[i]);
}

TEST(wire_process_rejects_inconsistent_payloads)
{
    phylo::host::ProcessRequest req;
    req.frames = 2;
    req.channels = 2;
    req.audio = {0.0f, 0.0f, 0.0f, 0.0f};
    auto good = phylo::host::encodeProcess(req);

    phylo::host::ProcessRequest out;
    // Trailing byte.
    auto extra = good;
    extra.push_back(0);
    CHECK(!phylo::host::decodeProcess(extra, out));
    // Claims more frames than the audio carries.
    auto lying = good;
    lying[0] = 3;
    CHECK(!phylo::host::decodeProcess(lying, out));
    // MIDI count larger than the payload can hold.
    auto midiLie = good;
    midiLie[8] = 0xFF;
    midiLie[9] = 0xFF;
    CHECK(!phylo::host::decodeProcess(midiLie, out));
    // Too short to hold the header.
    CHECK(!phylo::host::decodeProcess({1, 2, 3}, out));
}

TEST(wire_state_frames_carry_raw_bytes_and_unknown_types_past_the_new_end_are_refused)
{
    // Plugin state is binary, so it must survive unchanged, including 0x00 and 0xFF.
    const std::vector<std::uint8_t> state = {0x00, 0xFF, 0x7F, 0x80, 0x0A, 0x00};
    phylo::host::FrameDecoder dec;
    const auto get = phylo::host::encodeFrame(phylo::host::MsgType::GetState, {});
    const auto set = phylo::host::encodeFrame(phylo::host::MsgType::SetState, state);
    dec.feed(get.data(), get.size());
    dec.feed(set.data(), set.size());
    phylo::host::Frame a, b;
    CHECK(dec.next(a) && a.type == phylo::host::MsgType::GetState && a.payload.empty());
    CHECK(dec.next(b) && b.type == phylo::host::MsgType::SetState && b.payload == state);

    const std::vector<std::uint8_t> bad = {0x00, 0x00, 0x00, 0x00, 0x0C}; // type 12: not defined
    phylo::host::FrameDecoder dec2;
    dec2.feed(bad.data(), bad.size());
    phylo::host::Frame f;
    CHECK(!dec2.next(f));
    CHECK(dec2.failed());
}

TEST(wire_hello_audio_and_text_round_trip)
{
    std::uint32_t v = 0, sr = 0, mb = 0;
    CHECK(phylo::host::decodeHello(phylo::host::encodeHello(1, 48000, 512), v, sr, mb));
    CHECK(v == 1 && sr == 48000 && mb == 512);
    CHECK(!phylo::host::decodeHello({1, 0, 0, 0, 2}, v, sr, mb));

    const std::vector<float> audio = {0.5f, -0.5f};
    std::uint32_t f = 0, c = 0;
    std::vector<float> back;
    CHECK(phylo::host::decodeAudio(phylo::host::encodeAudio(1, 2, audio), f, c, back));
    CHECK(f == 1 && c == 2 && back == audio);
    CHECK(!phylo::host::decodeAudio({1, 0, 0, 0, 2, 0, 0, 0, 0, 0}, f, c, back));

    std::string text;
    CHECK(phylo::host::decodeText(phylo::host::encodeText("C:\\Plugins\\x.vst3"), text));
    CHECK(text == "C:\\Plugins\\x.vst3");
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
