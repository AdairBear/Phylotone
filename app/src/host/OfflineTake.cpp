#include "OfflineTake.h"

#include <algorithm>

phylo::host::ProcessRequest buildBlock(const std::vector<phylo::MidiOut>& midi, std::size_t frames)
{
    phylo::host::ProcessRequest req;
    req.frames = static_cast<std::uint32_t>(frames);
    req.channels = 2;
    req.audio.assign(frames * 2, 0.0f);
    for (const auto& m : midi)
        req.midi.push_back({ static_cast<std::uint32_t>(m.sampleOffset), m.status, m.data1, m.data2 });
    return req;
}

OfflineTake renderPatternOffline(const phylo::Pattern& pattern, double bpm, int sampleRate,
                                 std::size_t frames, const BlockRenderer& renderBlock,
                                 std::size_t blockFrames)
{
    phylo::Sequencer seq;
    seq.setSampleRate(sampleRate);
    seq.setTempo(bpm);
    seq.setPattern(pattern);
    seq.play();

    OfflineTake take;
    take.stereo.reserve(frames * 2);
    std::vector<phylo::MidiOut> midi;
    std::vector<float> block;

    std::size_t start = 0;
    while (start < frames)
    {
        const std::size_t n = std::min(blockFrames, frames - start);

        midi.clear();
        seq.process(static_cast<int>(n), midi);

        const auto req = buildBlock(midi, n);
        for (const auto& m : midi)
        {
            const bool noteOn = (m.status & 0xF0) == 0x90 && m.data2 > 0;
            if (noteOn)
                take.noteOnFrames.push_back(start + static_cast<std::size_t>(m.sampleOffset));
        }

        block.assign(n * 2, 0.0f);
        renderBlock(req, block);
        block.resize(n * 2, 0.0f); // a short answer is padded with silence
        take.stereo.insert(take.stereo.end(), block.begin(), block.end());

        start += n;
    }
    return take;
}
