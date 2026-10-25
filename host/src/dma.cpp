/*
 * Source DMA transfers, walked at once (dma.h).
 */

#include "ps2host/dma.h"

#include <cstdint>
#include <cstring>

#include "ps2host/memory.h"

namespace ps2host {

std::uint32_t g_dma_source = 0;


namespace {

constexpr std::uint32_t kScratchpad = 0x70000000u;

std::uint32_t Address(std::uint64_t tag) {
    const std::uint32_t address = std::uint32_t(tag >> 32) & 0x7FFFFFF0u;
    return (tag >> 63) ? kScratchpad | (address & 0x3FF0u) : address;
}

// A channel's MADR or TADR with bit 31 (SPR) set names the scratchpad, at its
// low 14 bits, as a tag's ADDR does (Address) - not kseg0's view of main RAM.
// Tekken 5's boot screens send their packets from the scratchpad: read from
// main RAM they were zeros, and the screens black.
std::uint32_t Source(std::uint32_t address) {
    return (address >> 31) ? kScratchpad | (address & 0x3FF0u) : address;
}

// The quadwords taken: all of them, unless the receiver asked to stop (`stop`):
// then the quadword it stopped in counts as taken and the rest of it is held.
std::uint32_t Send(std::uint32_t from, std::uint32_t quadwords, DmaOutcome& outcome, const DmaBlockSink& words,
                   DmaStop* stop) {
    Memory& m = GuestMemory();
    from = Source(from);
    auto stopped = [&](std::uint32_t taken, const std::uint32_t* run) {
        const std::uint32_t taken_quadwords = (taken + 3) / 4;
        for (std::uint32_t k = taken; k < 4 * taken_quadwords; ++k)
            stop->held.push_back(run ? run[k] : std::uint32_t(m.Read(from + 4 * k, 4)));
        outcome.quadwords += taken_quadwords;
        outcome.stopped = true;
        return taken_quadwords;
    };
    // Main RAM or the scratchpad, as DMA reads them: one pointer for the lot
    // (a word at a time through Read cost a tenth of a frame). Anything with
    // register hooks goes word by word.
    const std::uint32_t physical = from & 0x1FFFFFFFu;
    const bool plain = physical < 0x10000000u || (from >> 28) == 0x7;
    const std::uint8_t* p = plain && quadwords ? m.Translate(from, 16 * quadwords) : nullptr;
    if (p && reinterpret_cast<std::uintptr_t>(p) % 4 == 0) {
        const auto* run = reinterpret_cast<const std::uint32_t*>(p);
        words(run, 4 * quadwords, from);
        if (stop && stop->requested) return stopped(stop->taken, run);
    } else {
        for (std::uint32_t i = 0; i < 4 * quadwords; ++i) {
            const std::uint32_t value = std::uint32_t(m.Read(from + 4 * i, 4));
            words(&value, 1, from + 4 * i);
            if (stop && stop->requested) return stopped(i + stop->taken, nullptr);
        }
    }
    outcome.quadwords += quadwords;
    return quadwords;
}

}  // namespace

DmaOutcome SendDma(DmaChannelState& ch, const std::function<void(std::uint32_t)>& word,
                   std::uint32_t max_tags, bool tags) {
    return SendDmaBlocks(ch, [&word](const std::uint32_t* words, std::uint32_t count, std::uint32_t from) {
        for (std::uint32_t i = 0; i < count; ++i) {
            g_dma_source = from + 4 * i;
            word(words[i]);
        }
    }, max_tags, tags);
}

DmaOutcome SendDmaBlocks(DmaChannelState& ch, const DmaBlockSink& word, std::uint32_t max_tags,
                         bool tags, DmaStop* stop) {
    DmaOutcome outcome;
    const std::uint32_t mode = (ch.chcr >> 2) & 3;
    // Normal mode, or a chain resumed with QWC still owed: data from MADR first.
    if (ch.qwc) {
        const std::uint32_t sent = Send(ch.madr, ch.qwc, outcome, word, stop);
        ch.madr += 16 * sent;
        ch.qwc -= sent;
        if (outcome.stopped) {
            stop->ended = mode != 1;                // normal mode: that was all of it
            return outcome;
        }
    }
    if (mode != 1) {
        outcome.ended = true;
        return outcome;
    }
    // TTE: the VIFs take a tag's upper half as VIFcodes; the GIF does not. Sent
    // there, the zeros ahead of a packet misaligned its GIFtag - Resident Evil
    // Outbreak starts PATH3 with TTE set in a scene after a door, its frame's
    // register packet (ZBUF's depth clear among them) was lost, and the scene
    // was drawn against a depth buffer never cleared: black.
    const bool transfer_tags = tags && ((ch.chcr >> 6) & 1), irq_ends = (ch.chcr >> 7) & 1;
    std::uint32_t depth = (ch.chcr >> 4) & 3;       // ASP: call stack depth
    Memory& m = GuestMemory();
    while (outcome.tags < max_tags) {
        const std::uint64_t tag = m.Read(Source(ch.tadr), 8);
        ++outcome.tags;
        ch.chcr = (ch.chcr & 0xFFFFu) | (std::uint32_t(tag) & 0xFFFF0000u);     // TAG
        bool stopped_in_tag = false;
        if (transfer_tags) {
            const std::uint32_t high[2] = {std::uint32_t(m.Read(Source(ch.tadr) + 8, 4)),
                                           std::uint32_t(m.Read(Source(ch.tadr) + 12, 4))};
            word(high, 2, ch.tadr + 8);
            if (stop && stop->requested) {          // in the tag's VIFcodes: its data still to come
                stop->held.assign(high + stop->taken, high + 2);
                stopped_in_tag = true;
            }
        }
        const std::uint32_t qwc = std::uint32_t(tag) & 0xFFFFu;
        const std::uint32_t id = std::uint32_t(tag >> 28) & 7;
        const std::uint32_t after = ch.tadr + 16;
        bool end = false;
        std::uint32_t data = after;
        switch (id) {
            case 0: data = Address(tag); end = true; break;                         // refe
            case 1: ch.tadr = after + 16 * qwc; break;                              // cnt
            case 2: ch.tadr = Address(tag); break;                                  // next
            case 3: case 4: data = Address(tag); ch.tadr = after; break;            // ref, refs
            case 5:                                                                 // call
                if (depth == 0) ch.asr0 = after + 16 * qwc;
                else ch.asr1 = after + 16 * qwc;
                if (depth < 2) ++depth;
                ch.tadr = Address(tag);
                break;
            case 6:                                                                 // ret
                if (depth == 0) end = true;
                else ch.tadr = (--depth == 0) ? ch.asr0 : ch.asr1;
                break;
            case 7: end = true; break;                                              // end
        }
        const bool last = end || (irq_ends && (tag >> 31) & 1);
        if (stopped_in_tag) {
            ch.madr = data;
            ch.qwc = qwc;
            stop->ended = last;
            outcome.stopped = true;
            break;
        }
        const std::uint32_t sent = Send(data, qwc, outcome, word, stop);
        ch.madr = data + 16 * sent;
        if (outcome.stopped) {
            ch.qwc = qwc - sent;
            stop->ended = last;
            break;
        }
        if (last) {
            outcome.ended = true;
            break;
        }
    }
    ch.chcr = (ch.chcr & ~0x30u) | (depth << 4);
    return outcome;
}

}  // namespace ps2host
