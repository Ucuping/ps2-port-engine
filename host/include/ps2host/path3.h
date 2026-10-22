/*
 * PATH3 held back by VIF1's MSKPATH3 (devices.cpp).
 *
 * GIF DMA data waits here while PATH3 is masked. A mask takes effect only at
 * the end of the GIF packet PATH3 is in - the one whose tag has EOP set - so
 * unmasking and masking again at once lets exactly one EOP-ended group of
 * packets through: Release(false). Release(true) lets all of it go (time
 * passing with PATH3 unmasked, or FLUSHA waiting for it).
 */

#ifndef PS2HOST_PATH3_H
#define PS2HOST_PATH3_H

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace ps2host {

class Path3Queue {
public:
    void Push(std::uint32_t word) {
        if (spans_.empty() || !spans_.back().own || spans_.back().own->size() != spans_.back().offset + spans_.back().count) {
            Span s;
            s.own = std::make_shared<std::vector<std::uint32_t>>();
            spans_.push_back(std::move(s));
        }
        Span& last = spans_.back();
        last.own->push_back(word);
        ++last.count;
        size_ += 1;
    }
    /* Copied: the words may change once this returns. */
    void Push(const std::uint32_t* words, std::size_t count) {
        if (!count) return;
        Span s;
        s.own = std::make_shared<std::vector<std::uint32_t>>(words, words + count);
        s.count = count;
        spans_.push_back(std::move(s));
        size_ += count;
    }
    /* Not copied: the words stay as they are while `owner` lives (VIF1's
       worker's copy of a transfer, devices.cpp - Burnout 3 holds a
       picture's textures here, 160,000 words, a copy each was most of what
       PATH3 cost the worker). */
    void Push(const std::uint32_t* words, std::size_t count, std::shared_ptr<const void> owner) {
        if (!count) return;
        Span s;
        s.data = words;
        s.count = count;
        s.owner = std::move(owner);
        spans_.push_back(std::move(s));
        size_ += count;
    }
    bool empty() const { return size_ == 0; }
    std::size_t size() const { return size_; }

    /* Hands words to `sink` (runs: words, count - a packet may be split
       between them), up to the end of the first packet with EOP set (or all
       of them). */
    template <typename Sink>
    void ReleaseBlock(bool all, Sink&& sink) {
        std::size_t end = size_;
        if (!all) {
            // The tags, read where they are: a quadword's four words may lie
            // in two spans.
            std::size_t at = 0;
            Cursor cursor{spans_};
            while (at + 4 <= size_) {
                const std::uint64_t low = std::uint64_t(cursor.At(at)) | std::uint64_t(cursor.At(at + 1)) << 32;
                const std::uint64_t nloop = low & 0x7FFF, eop = (low >> 15) & 1;
                const std::uint64_t flg = (low >> 58) & 3, nreg = (low >> 60) ? (low >> 60) : 16;
                const std::uint64_t qwords = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
                at += 4 + std::size_t(qwords) * 4;
                if (eop) break;
            }
            end = std::min(at, size_);
        }
        // Taken off before the sink runs: what it does may hold more (the
        // spans keep their words alive, wherever they are).
        std::vector<Span> going;
        std::size_t left = end;
        while (left) {
            Span& first = spans_.front();
            if (first.count <= left) {
                left -= first.count;
                going.push_back(std::move(first));
                spans_.pop_front();
            } else {
                Span part = first;
                part.count = left;
                going.push_back(std::move(part));
                first.offset += left;
                first.count -= left;
                left = 0;
            }
        }
        size_ -= end;
        for (const Span& s : going) sink(s.Words(), s.count);
    }

    /* The same, a word at a time. */
    template <typename Sink>
    void Release(bool all, Sink&& sink) {
        ReleaseBlock(all, [&](const std::uint32_t* words, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) sink(words[i]);
        });
    }

private:
    /* Words held: borrowed (data, kept by owner) or the queue's own. */
    struct Span {
        const std::uint32_t* data = nullptr;
        std::shared_ptr<const void> owner;
        std::shared_ptr<std::vector<std::uint32_t>> own;
        std::size_t offset = 0, count = 0;
        const std::uint32_t* Words() const { return (own ? own->data() : data) + offset; }
    };

    /* The word at an index, the indices asked for never going back. */
    struct Cursor {
        const std::deque<Span>& spans;
        std::size_t span = 0, base = 0;     // the span at `span` begins at word `base`
        std::uint32_t At(std::size_t index) {
            while (span < spans.size() && index >= base + spans[span].count) base += spans[span++].count;
            return span < spans.size() ? spans[span].Words()[index - base] : 0;
        }
    };

    std::deque<Span> spans_;
    std::size_t size_ = 0;
};

}  // namespace ps2host

#endif  /* PS2HOST_PATH3_H */
