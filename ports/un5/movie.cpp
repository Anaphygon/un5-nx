// SPDX-License-Identifier: GPL-3.0+
// UN5's movies (PSS/*.PSS), decoded on the host instead of on the IPU.
//
// The game plays a movie with Sony's libmpeg on two EE threads that the
// player start (0x1058F0) creates:
// - a reader (0x104020) streams the file from the disc through the IOP
//   (sceCdSt*), splits it with sceMpegDemuxPss, and pumps the PCM to the SPU2
//   block by block over sceSdRemote; it also watches skip and pause
//   (ctrl+0x34/0x35) and ends the movie (ctrl+0x3A = 2);
// - a decoder (0x101C00) takes a free picture slot, fills it with
//   sceMpegGetPicture (the IPU, RGBA32 macroblocks) and hands it to
//   0x104990, which uploads it to the GS and draws it over the frame, where
//   the game then draws its subtitles.
// On a PS2 that is cheap; here it was the slowest thing in the game: the
// reader polls the IOP some 1300 times a frame, and the IPU, the CD stream and
// the SPU2 transfers all run emulated (the movie mode in ps2-web-recomp's
// system.cpp only made it bearable, at a cost in pictures).
//
// Here both threads keep running the game's code, so skip, pause, the end of
// a movie, the display and the subtitles are the game's own, and only the
// leaves that touch the hardware are replaced (eerecomp --hooks, list in
// hooks.txt):
// - opening, reading and closing the CD stream start and stop a host thread
//   that reads the file from the disc image, splits it and decodes the
//   MPEG-2 video with libmpeg2 (third_party/libmpeg2) into RGBA pictures and
//   the PCM into a ring;
// - the demuxer and the PCM pump do nothing;
// - sceMpegGetPicture copies the next decoded picture into the game's slot
//   in the IPU's layout and colours, and sceMpegIsEnd says when the last one
//   was taken;
// - the sound starts, pauses and stops where the game's does, and the ring is
//   mixed into the SPU2's output (ps2::set_audio_mix).
// The pictures are shown at the game's own pace (its VBLANK handler); the
// sound runs on the mixer's samples from the moment the game starts it, which
// on the PS2 is also when the first picture shows.
//
// After Downhill Revived's native movie player (skuza-oss/downhill-revived,
// port/hle/movie.c, finding 055, GPL-3.0-or-later): the PSS layout, the
// demuxer and the sound's PTS offset follow it.
//
// UN5_MOVIE_NATIVE=0 leaves the movies to the emulated player.
#include <cstdint>
extern "C" {
#include "libmpeg2/mpeg2.h"
}

#include "ps2/disc.hpp"
#include "ps2/generated.hpp"
#include "ps2/hook.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------- decoder

constexpr int kRate = 48000;
constexpr int kSlots = 6;               // pictures decoded ahead (0.24 s at 25/s)
constexpr std::size_t kRing = 1u << 17; // sound frames (2.7 s)
constexpr std::size_t kRead = 256 * 1024;

struct Picture {
    std::vector<std::uint32_t> rgba; // width x height, IPU layout of a pixel
    std::int64_t index = -1;         // in display order; -1: free
};

class Decoder {
public:
    using Read = bool (*)(std::uint64_t offset, void *out, std::size_t bytes, void *ctx);

    ~Decoder() { stop(); }

    bool start(Read read, void *ctx, std::uint64_t size, bool audio) {
        stop();
        read_ = read;
        ctx_ = ctx;
        size_ = size;
        audio_ = audio;
        quit_ = ended_ = full_ = false;
        decoded_ = taken_ = 0;
        width_ = height_ = 0;
        ring_.assign(kRing * 2, 0);
        ring_write_ = ring_read_ = 0;
        position_ = 0;
        rate_ = kRate;
        audio_offset_ = audio_skip_ = 0;
        for (Picture &p : slots_) p.index = -1;
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        if (!thread_.joinable()) return;
        {
            std::lock_guard<std::mutex> hold(lock_);
            quit_ = true;
        }
        changed_.notify_all();
        thread_.join();
    }

    bool running() const { return thread_.joinable(); }

    // The next picture into out (width x height RGBA), waiting for the decoder
    // at most timeout_ms. False when the movie has no more.
    bool next(std::vector<std::uint32_t> &out, int &width, int &height, int timeout_ms) {
        std::unique_lock<std::mutex> hold(lock_);
        Picture &p = slots_[taken_ % kSlots];
        const std::int64_t want = taken_;
        changed_.wait_for(hold, std::chrono::milliseconds(timeout_ms), [&] { return p.index == want || ended_ || quit_; });
        if (p.index != want) return false;
        out.swap(p.rgba);
        p.rgba.resize(out.size());
        width = width_;
        height = height_;
        p.index = -1;
        ++taken_;
        hold.unlock();
        changed_.notify_all();
        return true;
    }

    // The file is read to its end (its last pictures may still be in slots).
    bool read_all() {
        std::lock_guard<std::mutex> hold(lock_);
        return ended_;
    }

    // Every picture has been taken (or none will come).
    bool finished() {
        std::lock_guard<std::mutex> hold(lock_);
        return ended_ && taken_ >= decoded_;
    }

    std::int64_t taken() {
        std::lock_guard<std::mutex> hold(lock_);
        return taken_;
    }

    // Sound frames buffered ahead of the mixer, or a large number once no
    // more can come before the game takes a picture: the file is all read, or
    // every slot holds a picture (the sound in the file sits after them).
    std::size_t sound_ahead() {
        std::lock_guard<std::mutex> hold(lock_);
        if (ended_ || full_) return kRing;
        return static_cast<std::size_t>(ring_write_ - ring_read_);
    }

    // Waits at most timeout_ms for frames of sound ahead (or for no more to
    // come before the game takes a picture); true if they are there.
    bool wait_sound(std::size_t frames, int timeout_ms) {
        std::unique_lock<std::mutex> hold(lock_);
        return changed_.wait_for(hold, std::chrono::milliseconds(timeout_ms), [&] {
            return ended_ || full_ || quit_ || static_cast<std::size_t>(ring_write_ - ring_read_) >= frames;
        });
    }

    // Adds up to frames of the movie's sound to stereo (48 kHz), resampled
    // linearly from the movie's rate (NARUTO10's is 44.1 kHz: the PS2 played
    // it 9% fast, ahead of its pictures, as the emulated player does too);
    // silence where it has none.
    void mix(std::int16_t *stereo, std::size_t frames) {
        std::lock_guard<std::mutex> hold(lock_);
        const double step = static_cast<double>(rate_) / kRate;
        for (std::size_t n = 0; n < frames; ++n, ++clock_) {
            if (clock_ < audio_offset_) continue;
            const std::int64_t at = static_cast<std::int64_t>(position_);
            if (at < ring_read_) ring_read_ = at; // never behind what is still in the ring
            if (at + 1 >= ring_write_) break;
            const double frac = position_ - static_cast<double>(at);
            const std::int16_t *a = &ring_[(at & (kRing - 1)) * 2], *b = &ring_[((at + 1) & (kRing - 1)) * 2];
            for (int ch = 0; ch < 2; ++ch) {
                const int v = static_cast<int>(a[ch] + (b[ch] - a[ch]) * frac);
                stereo[n * 2 + ch] = static_cast<std::int16_t>(std::clamp(stereo[n * 2 + ch] + v, -32768, 32767));
            }
            position_ += step;
            ring_read_ = static_cast<std::int64_t>(position_);
        }
    }

    void restart_clock() {
        std::lock_guard<std::mutex> hold(lock_);
        clock_ = 0;
        position_ = static_cast<double>(ring_read_ + audio_skip_);
    }

private:
    struct Demux {
        std::vector<std::uint8_t> buf;
        std::size_t start = 0, end = 0;
        std::uint64_t offset = 0;
        mpeg2dec_t *dec = nullptr;
        std::int64_t pts_video = -1, pts_audio = -1;
        std::uint8_t header[40]{};
        unsigned header_have = 0;
        bool audio_ok = false;
        std::uint32_t audio_left = 0;
        std::uint8_t block[0x400]{};
        unsigned block_have = 0;
    };

    bool refill(Demux &d, std::size_t want) {
        if (d.end - d.start >= want) return true;
        std::memmove(d.buf.data(), d.buf.data() + d.start, d.end - d.start);
        d.end -= d.start;
        d.start = 0;
        while (d.offset < size_ && d.end < kRead) {
            // Whole sectors from the disc image, as the reader hands them.
            std::size_t n = std::min<std::uint64_t>(kRead - d.end, size_ - d.offset);
            if (!read_(d.offset, d.buf.data() + d.end, n, ctx_)) {
                d.offset = size_;
                break;
            }
            d.end += n;
            d.offset += n;
        }
        return d.end - d.start >= want;
    }

    // ITU-R BT.601 as the IPU's CSC computes it (ps2-web-recomp ipu.cpp:
    // convert), alpha 0x80, one chroma sample per 2x2 pixels.
    static void to_rgba(std::uint32_t *out, int w, int h, std::uint8_t *const planes[3], int ys, int cs) {
        for (int y = 0; y < h; ++y) {
            const std::uint8_t *luma = planes[0] + y * ys, *cb = planes[1] + (y >> 1) * cs, *cr = planes[2] + (y >> 1) * cs;
            std::uint32_t *o = out + y * w;
            for (int x = 0; x < w; ++x) {
                const int lum = 149 * std::max(0, luma[x] - 16);
                const int u = cb[x >> 1] - 128, v = cr[x >> 1] - 128;
                const int r = std::clamp((lum + 204 * v + 64) >> 7, 0, 255);
                const int g = std::clamp((lum - 50 * u - 104 * v + 64) >> 7, 0, 255);
                const int b = std::clamp((lum + 258 * u + 64) >> 7, 0, 255);
                o[x] = static_cast<std::uint32_t>(r | g << 8 | b << 16) | 0x80000000u;
            }
        }
    }

    void picture_out(const mpeg2_info_t *info) {
        const mpeg2_sequence_t *s = info->sequence;
        if (!info->display_fbuf || !s) return;
        const int w = static_cast<int>(s->picture_width), h = static_cast<int>(s->picture_height);
        std::unique_lock<std::mutex> hold(lock_);
        const std::int64_t k = decoded_;
        Picture &p = slots_[k % kSlots];
        full_ = p.index >= 0;
        if (full_) changed_.notify_all();
        changed_.wait(hold, [&] { return p.index < 0 || quit_; });
        full_ = false;
        if (quit_) return;
        width_ = w;
        height_ = h;
        hold.unlock();
        p.rgba.resize(static_cast<std::size_t>(w) * h);
        to_rgba(p.rgba.data(), w, h, info->display_fbuf->buf, static_cast<int>(s->width), static_cast<int>(s->chroma_width));
        hold.lock();
        p.index = k;
        decoded_ = k + 1;
        hold.unlock();
        changed_.notify_all();
    }

    void video(Demux &d, std::uint8_t *p, std::uint8_t *end) {
        mpeg2_buffer(d.dec, p, end);
        for (;;) {
            const mpeg2_info_t *info = mpeg2_info(d.dec);
            const mpeg2_state_t state = mpeg2_parse(d.dec);
            if (state == STATE_BUFFER || quit_) return;
            if (state == STATE_SLICE || state == STATE_END || state == STATE_INVALID_END) picture_out(info);
        }
    }

    static std::uint32_t le32(const std::uint8_t *p) {
        return p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24;
    }

    // One interleave block: 0x200 bytes of left, then 0x200 of right. The
    // pictures hold the decoder back, so the sound is never far ahead; if it
    // were, its oldest frames go.
    void sound_block(Demux &d) {
        constexpr std::size_t kFrames = 0x100;
        std::lock_guard<std::mutex> hold(lock_);
        if (ring_write_ + kFrames - ring_read_ > kRing) {
            ring_read_ = ring_write_ + kFrames - kRing;
            position_ = std::max(position_, static_cast<double>(ring_read_));
        }
        const auto *l = reinterpret_cast<const std::int16_t *>(d.block), *r = reinterpret_cast<const std::int16_t *>(d.block + 0x200);
        for (std::size_t i = 0; i < kFrames; ++i) {
            std::int16_t *o = &ring_[((ring_write_ + i) & (kRing - 1)) * 2];
            o[0] = l[i];
            o[1] = r[i];
        }
        ring_write_ += kFrames;
        changed_.notify_all();
    }

    // "SShd" (type 1 PCM16LE, 48 kHz, 2 channels, interleave 0x200), "SSbd", then blocks.
    void sound(Demux &d, const std::uint8_t *p, std::size_t n) {
        while (n) {
            if (d.header_have < sizeof d.header) {
                const std::size_t take = std::min(sizeof d.header - d.header_have, n);
                std::memcpy(d.header + d.header_have, p, take);
                d.header_have += static_cast<unsigned>(take);
                p += take;
                n -= take;
                if (d.header_have == sizeof d.header) {
                    const std::uint8_t *h = d.header;
                    const std::uint32_t rate = le32(h + 12);
                    d.audio_ok = !std::memcmp(h, "SShd", 4) && !std::memcmp(h + 32, "SSbd", 4) && le32(h + 8) == 1 &&
                                 rate >= 8000 && rate <= 96000 && le32(h + 16) == 2 && le32(h + 20) == 0x200;
                    d.audio_left = le32(h + 36);
                    if (d.audio_ok) {
                        std::lock_guard<std::mutex> hold(lock_);
                        rate_ = rate;
                    } else {
                        std::fprintf(stderr, "[movie] sound is not 16-bit stereo PCM (type %u, %u Hz): silent\n", le32(h + 8), rate);
                    }
                }
                continue;
            }
            if (!d.audio_ok) return;
            std::size_t take = std::min<std::size_t>({sizeof d.block - d.block_have, n, d.audio_left});
            if (!take) return;
            std::memcpy(d.block + d.block_have, p, take);
            d.block_have += static_cast<unsigned>(take);
            d.audio_left -= static_cast<std::uint32_t>(take);
            p += take;
            n -= take;
            if (d.block_have == sizeof d.block) {
                sound_block(d);
                d.block_have = 0;
            }
        }
    }

    static std::int64_t pts(const std::uint8_t *h) {
        return static_cast<std::int64_t>((h[0] >> 1) & 7) << 30 | h[1] << 22 | (h[2] >> 1) << 15 | h[3] << 7 | h[4] >> 1;
    }

    // The MPEG-2 program stream: pack headers, video PES 0xE0, Sony's private
    // stream 1 (0xBD, sub-header ff a0 00 00) for the sound; the rest skipped.
    void demux(Demux &d) {
        while (!quit_ && refill(d, 6)) {
            std::uint8_t *p = d.buf.data() + d.start;
            if (p[0] || p[1] || p[2] != 1) {
                ++d.start;
                continue;
            }
            const int id = p[3];
            if (id == 0xB9) break;
            if (id == 0xBA) {
                if (!refill(d, 14)) break;
                p = d.buf.data() + d.start;
                d.start += (p[4] >> 6) == 1 ? 14 + (p[13] & 7) : 12;
                continue;
            }
            if (id < 0xBB) {
                d.start += 4;
                continue;
            }
            const std::size_t length = static_cast<std::size_t>(p[4]) << 8 | p[5];
            if (!refill(d, 6 + length)) break;
            p = d.buf.data() + d.start;
            std::uint8_t *body = p + 6, *end = body + length;
            d.start += 6 + length;
            if ((id != 0xE0 && id != 0xBD) || length < 3 || (body[0] & 0xC0) != 0x80) continue;
            std::uint8_t *payload = body + 3 + body[2];
            if (payload > end) continue;
            const std::int64_t t = (body[1] & 0x80) && body[2] >= 5 ? pts(body + 3) : -1;
            if (id == 0xE0) {
                if (t >= 0 && d.pts_video < 0) d.pts_video = t;
                video(d, payload, end);
            } else if (audio_ && end - payload > 4 && payload[1] == 0xA0) {
                if (t >= 0 && d.pts_audio < 0) {
                    d.pts_audio = t;
                    if (d.pts_video >= 0) {
                        // Sound that starts after the first picture waits for
                        // it; sound that starts before it (OPENING's and
                        // NARUTO10's, by 80 ms) is skipped up to it, as the
                        // PS2's player has it (checked against the emulated
                        // player's output: the same lag otherwise).
                        std::lock_guard<std::mutex> hold(lock_);
                        audio_offset_ = std::max<std::int64_t>(0, (t - d.pts_video) * kRate / 90000);
                        audio_skip_ = std::max<std::int64_t>(0, (d.pts_video - t) * rate_ / 90000);
                    }
                }
                sound(d, payload + 4, static_cast<std::size_t>(end - payload - 4));
            }
        }
        if (!quit_) {
            static std::uint8_t sequence_end[] = {0, 0, 1, 0xB7};
            video(d, sequence_end, sequence_end + sizeof sequence_end);
        }
    }

    void run() {
        Demux d;
        d.buf.resize(kRead);
        d.dec = mpeg2_init();
        if (d.dec) {
            demux(d);
            mpeg2_close(d.dec);
        }
        {
            std::lock_guard<std::mutex> hold(lock_);
            ended_ = true;
        }
        changed_.notify_all();
    }

    Read read_ = nullptr;
    void *ctx_ = nullptr;
    std::uint64_t size_ = 0;
    bool audio_ = true;
    std::thread thread_;
    std::mutex lock_;
    std::condition_variable changed_;
    std::atomic<bool> quit_{false};
    bool ended_ = false, full_ = false;
    Picture slots_[kSlots];
    std::int64_t decoded_ = 0, taken_ = 0;
    int width_ = 0, height_ = 0;
    std::vector<std::int16_t> ring_;
    std::int64_t ring_write_ = 0, ring_read_ = 0, clock_ = 0, audio_offset_ = 0;
    double position_ = 0; // in the movie's sound frames
    std::int64_t audio_skip_ = 0;
    std::uint32_t rate_ = kRate;
};

#ifndef UN5_MOVIE_TEST

// ---------------------------------------------------------------- the game

using ps2::Cpu;

// The player's globals (gp-relative in the game, at fixed addresses).
constexpr std::uint32_t kCtrl = 0x00617B3Cu; // the player: name at +0x3C, flags +0x34..+0x3A, stream +0x40
constexpr std::uint32_t kMpeg = 0x00617B14u; // sceMpeg: width +0, height +4, frame count +8
constexpr std::uint32_t kSound = 0x00617B18u; // the PCM streamer, state at +0
constexpr std::uint32_t kQueue = 0x00617B28u;   // the picture slots: state, picture, packet (12 bytes each); count +0x1C
constexpr std::uint32_t kDisplay = 0x00617B40u; // the VBLANK handler shows pictures
constexpr std::uint32_t kShown = 0x00617B48u;   // the oldest picture has had its fields

Decoder g_decoder;
std::uint32_t g_sector = 0;
std::uint32_t g_left = 0; // what the reader thinks is left to split (see demux)
bool g_sound_on = false;
bool g_sound_started = false; // this movie's sound has begun (a resume keeps its clock)
std::vector<std::uint32_t> g_picture;

bool enabled() {
    static const bool on = [] {
        const char *e = std::getenv("UN5_MOVIE_NATIVE");
        return !e || e[0] != '0';
    }();
    return on;
}

std::uint32_t ld32(std::uint32_t a) {
    std::uint32_t v = 0;
    if (const auto *p = ps2::fast<std::uint8_t>(a & 0x1FFFFFFFu)) std::memcpy(&v, p, 4);
    return v;
}

void st32(std::uint32_t a, std::uint32_t v) {
    if (auto *p = ps2::fast<std::uint8_t>(a & 0x1FFFFFFFu)) std::memcpy(p, &v, 4);
}

std::uint8_t ld8(std::uint32_t a) {
    const auto *p = ps2::fast<std::uint8_t>(a & 0x1FFFFFFFu);
    return p ? *p : 0;
}

std::string ld_string(std::uint32_t a) {
    std::string s;
    for (char ch; s.size() < 64 && (ch = static_cast<char>(ld8(a + static_cast<std::uint32_t>(s.size())))) != 0;) s += ch;
    return s;
}

// UN5_MOVIE_LOG=1: each stand-in's calls, with the player's flags.
void trace(const char *what) {
    static const bool on = std::getenv("UN5_MOVIE_LOG") != nullptr;
    if (!on) return;
    const std::uint32_t ctrl = ld32(kCtrl), mp = ld32(kMpeg);
    const std::uint32_t queue = ld32(kQueue);
    std::fprintf(stderr, "[movie] field %6llu %-12s skip %u pause %u end %u | pictures %u state %u | queue %u (slot states %u %u) shown %u display %u | taken %lld finished %d\n",
                 static_cast<unsigned long long>(ps2::now() / 5898240u), what, ld8(ctrl + 0x34), ld8(ctrl + 0x35), ld8(ctrl + 0x3A), mp ? ld32(mp + 8) : 0, mp ? ld32(mp + 0xA8) : 0,
                 queue ? ld32(queue + 0x1C) : 0, queue ? ld32(queue) : 0, queue ? ld32(queue + 0xC) : 0, ld32(kShown), ld32(kDisplay),
                 static_cast<long long>(g_decoder.taken()), g_decoder.finished() ? 1 : 0);
}

// Calls that come every pass of the reader's loop: traced once a second.
void trace_often(const char *what, int &count) {
    if (count++ % 50 == 0) trace(what);
}

bool ret(Cpu &c, std::uint32_t v0) {
    c.r[2].sd[0] = static_cast<std::int32_t>(v0);
    c.pc = c.r[31].w[0];
    return true;
}

bool read_disc(std::uint64_t offset, void *out, std::size_t bytes, void *) {
    // offset is from the file's start and a whole number of sectors but for
    // the tail; disc::read wants sectors.
    const std::uint32_t first = g_sector + static_cast<std::uint32_t>(offset / ps2::disc::kSector);
    const std::uint32_t count = static_cast<std::uint32_t>((bytes + ps2::disc::kSector - 1) / ps2::disc::kSector);
    if (bytes % ps2::disc::kSector == 0) return ps2::disc::read(first, count, out);
    std::vector<std::uint8_t> tmp(static_cast<std::size_t>(count) * ps2::disc::kSector);
    if (!ps2::disc::read(first, count, tmp.data())) return false;
    std::memcpy(out, tmp.data(), bytes);
    return true;
}

void mix(std::int16_t *stereo, std::size_t frames) {
    if (g_sound_on) g_decoder.mix(stereo, frames);
}

// 0x103AD0, opening the stream (p = ctrl+0x40, a1 = the file name): the host
// decoder starts instead. The stream's size goes where the reader counts
// down what is left to split (p[0], p[2]); it never gets there (the demuxer
// takes nothing), and the reader ends when the decoder says the pictures are
// over (sceMpegIsEnd, then the decoder thread's state 3).
bool stream_open(Cpu &c) {
    if (!enabled()) {
        trace("open (PS2)");
        return false;
    }
    const std::uint32_t p = c.r[4].w[0];
    const std::string name = ld_string(c.r[5].w[0]);
    std::string upper = name;
    for (char &ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    const auto entry = ps2::disc::find("PSS/" + upper);
    const std::uint32_t ctrl = ld32(kCtrl);
    const bool sound = ld8(ctrl + 0x36) != 0;
    if (entry) {
        g_sector = entry->sector;
        g_decoder.start(read_disc, nullptr, entry->size, sound);
        std::fprintf(stderr, "[movie] %s natively (%u bytes, sound %s)\n", upper.c_str(), entry->size, sound ? "on" : "off");
    } else {
        std::fprintf(stderr, "[movie] %s is not on the disc\n", upper.c_str());
    }
    const std::uint32_t size = entry ? entry->size : 0x10000u;
    st32(p, size);
    st32(p + 8, size);
    g_left = size;
    st32(p + 0x2C, 0);
    for (std::uint32_t k = 0x28; k < 0x2B; ++k)
        if (auto *b = ps2::fast<std::uint8_t>(p + k)) *b = 0;
    g_sound_on = g_sound_started = false;
    ps2::set_audio_mix(mix);
    trace("open");
    if (std::getenv("UN5_MOVIE_LOG"))
        std::fprintf(stderr, "[movie] player 0x%08X, sceMpeg 0x%08X, slots 0x%08X\n", ld32(kCtrl), ld32(kMpeg), ld32(kQueue));
    return ret(c, 1);
}

// 0x103A20 (sceCdStRead(p, buffer, bytes) into the reader's ring): says it
// read the whole sectors asked for, as the game's does, and copies nothing.
// The reader counts the file down with what the demuxer takes, and its loop
// ends when nothing is left: only then does it let the last picture leave
// the screen (ctrl+0x3A = 1, 0x105120) and the decoder thread finish. On a
// PS2 the file is split long before its last pictures are decoded, and that
// order matters: the decoder thread ends in a loop that never yields
// (0x101D7C, until the slots are empty), so the reader, of the same priority,
// would never run again to set 0x3A. So the demuxer (0x15D820) takes nothing
// until the host decoder has read the whole file, then all of it.
bool demux(Cpu &c) {
    if (!enabled() || !g_decoder.running()) return false;
    std::uint32_t took = 0;
    if (g_decoder.read_all()) {
        took = g_left;
        g_left = 0;
    }
    static int n;
    if (took) trace("demux: all");
    else trace_often("demux", n);
    return ret(c, took);
}

bool stream_read(Cpu &c) {
    if (!enabled() || !g_decoder.running()) return false;
    // Traced when the picture slots change.
    static std::uint64_t last;
    const std::uint32_t queue = ld32(kQueue);
    const std::uint64_t now = queue ? (static_cast<std::uint64_t>(ld32(queue + 0x1C)) << 48 | static_cast<std::uint64_t>(ld32(queue)) << 40 |
                                       static_cast<std::uint64_t>(ld32(queue + 0xC)) << 32 | ld32(kShown) << 24 | ld8(ld32(kCtrl) + 0x3A) << 16 | g_decoder.taken())
                                    : 0;
    if (now != last) trace("read");
    last = now;
    return ret(c, c.r[6].w[0] >> 11 << 11);
}

// 0x103CF0 and 0x103D30: when the game wants the disc for something else
// (0x103C20, every pass of the reader), the stream is paused until the IOP
// says it stopped, then restarted until it says it runs. There is no stream:
// nothing to wait for.
bool stream_pause(Cpu &c) { return enabled() && g_decoder.running() ? ret(c, 0) : false; }
bool stream_resume(Cpu &c) { return enabled() && g_decoder.running() ? ret(c, 0) : false; }

// 0x1007D0, the player threads' yield: RotateThreadReadyQueue(0x7D), which
// only lets threads of their own priority run. On a PS2 they mostly sleep in
// the IOP and IPU calls between yields; without those calls their loops would
// spin and starve the game's lower-priority threads (the end of a movie hung
// on one). So a yield waits for the next field instead (0x1D5540(1), which the
// player's threads use themselves): the reader then passes once a field,
// the decoder looks for a free slot once a field.
bool yield(Cpu &c) {
    if (!enabled() || !g_decoder.running()) return false;
    c.r[4].sd[0] = 1;
    c.pc = 0x001D5540u; // returns to the yield's caller
    return true;
}

// 0x103A90, closing the stream.
bool stream_close(Cpu &c) {
    if (!enabled() || !g_decoder.running()) return false;
    g_sound_on = false;
    trace("close");
    g_decoder.stop();
    return ret(c, 0);
}

// 0x100C20, pumping PCM to the SPU2: the mixer takes it from the ring.
bool sound_pump(Cpu &c) { return enabled() && g_decoder.running() ? ret(c, 0) : false; }

// 0x104430, "is there sound enough to start": the first picture is shown once it says so.
bool sound_ready(Cpu &c) {
    if (!enabled() || !g_decoder.running()) return false;
    const std::uint32_t ctrl = ld32(kCtrl);
    // The decoder runs far ahead of the movie: waiting for it here costs a
    // moment once, and the movie starts at the same field however fast the
    // host is.
    const bool ready = ld8(ctrl + 0x36) == 0 || g_decoder.wait_sound(kRate / 4, 500);
    trace(ready ? "sound ready" : "sound wait");
    return ret(c, ready ? 1 : 0);
}

// 0x1011F0, sound on (also from 0x1011C0, the first start), 0x101290 paused,
// 0x101150 stopped; the streamer's state word as the game's code leaves it.
bool sound_start(Cpu &c) {
    if (!enabled() || !g_decoder.running()) return false;
    st32(c.r[4].w[0], 2);
    if (!g_sound_started) g_decoder.restart_clock();
    g_sound_started = g_sound_on = true;
    trace("sound on");
    return ret(c, 0);
}

bool sound_pause(Cpu &c) {
    if (!enabled() || !g_decoder.running()) return false;
    st32(c.r[4].w[0], 3);
    g_sound_on = false;
    trace("sound pause");
    return ret(c, 0);
}

bool sound_stop(Cpu &c) {
    if (!enabled() || !g_decoder.running()) return false;
    const std::uint32_t s = c.r[4].w[0];
    for (std::uint32_t word : {0u, 0xBu, 0xDu, 0xEu, 0x10u, 0x15u, 0x13u, 0x14u}) st32(s + word * 4, 0);
    g_sound_on = false;
    trace("sound stop");
    return ret(c, 0);
}

// 0x159628, sceMpegGetPicture(mp, rgb32, mbcount): the next picture in the
// IPU's output order, which 0x104990 reads column by column: the macroblock
// in column x, row y at (x * rows + y) * 0x400, 16 rows of 16 pixels each.
bool get_picture(Cpu &c) {
    if (!enabled() || !g_decoder.running()) {
        static int n;
        if (!enabled()) trace_often("picture (PS2)", n);
        return false;
    }
    const std::uint32_t mp = c.r[4].w[0], dst = c.r[5].w[0] & 0x1FFFFFFFu;
    // The frame count as libmpeg leaves it: 0 after the first two pictures,
    // then one more each (call i leaves i - 2; watched on the emulated
    // player). The decoder thread builds the slots' GS packets while it reads
    // 0 (0x101C00), and the subtitles time themselves by it (0x105420).
    st32(mp + 8, static_cast<std::uint32_t>(std::max<std::int64_t>(0, g_decoder.taken() - 1)));
    int w = 0, h = 0;
    // The decoder runs many times faster than the movie: a wait here is the
    // first picture's, or a slow card.
    if (!g_decoder.next(g_picture, w, h, 2000) || w <= 0 || h <= 0) {
        trace("no picture");
        return ret(c, 0);
    }
    const int cols = w / 16, rows = h / 16;
    auto *out = ps2::fast<std::uint8_t>(dst);
    if (!out || dst + static_cast<std::uint32_t>(cols * rows) * 0x400u > ps2::kRamSize) return ret(c, 0);
    for (int x = 0; x < cols; ++x)
        for (int y = 0; y < rows; ++y) {
            std::uint8_t *mb = out + static_cast<std::size_t>(x * rows + y) * 0x400;
            for (int line = 0; line < 16; ++line)
                std::memcpy(mb + line * 64, &g_picture[static_cast<std::size_t>(y * 16 + line) * w + x * 16], 64);
        }
    st32(mp, static_cast<std::uint32_t>(w));
    st32(mp + 4, static_cast<std::uint32_t>(h));
    return ret(c, 0);
}

// 0x159748, sceMpegIsEnd: once every decoded picture has been taken.
bool is_end(Cpu &c) {
    if (!enabled() || !g_decoder.running()) return false;
    static int n;
    trace_often("is end", n);
    if (g_decoder.finished()) trace("is end: yes");
    return ret(c, g_decoder.finished() ? 1 : 0);
}

struct Register {
    Register() {
        ps2::register_hook(0x00103AD0u, stream_open);
        ps2::register_hook(0x00103A20u, stream_read);
        ps2::register_hook(0x00103A90u, stream_close);
        ps2::register_hook(0x00103CF0u, stream_pause);
        ps2::register_hook(0x001007D0u, yield);
        ps2::register_hook(0x00103D30u, stream_resume);
        ps2::register_hook(0x0015D820u, demux);
        ps2::register_hook(0x00100C20u, sound_pump);
        ps2::register_hook(0x00104430u, sound_ready);
        ps2::register_hook(0x001011F0u, sound_start);
        ps2::register_hook(0x00101290u, sound_pause);
        ps2::register_hook(0x00101150u, sound_stop);
        ps2::register_hook(0x00159628u, get_picture);
        ps2::register_hook(0x00159748u, is_end);
    }
} g_register;

#else // UN5_MOVIE_TEST: decode a .PSS file, time it, write pictures and the sound.

std::FILE *g_file;

bool read_file(std::uint64_t offset, void *out, std::size_t bytes, void *) {
    fseeko(g_file, static_cast<off_t>(offset), SEEK_SET);
    return std::fread(out, 1, bytes, g_file) == bytes;
}

#endif

} // namespace

#ifdef UN5_MOVIE_TEST
#include <chrono>

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: movie_test <file.PSS> <out dir>\n");
        return 2;
    }
    g_file = std::fopen(argv[1], "rb");
    if (!g_file) return 1;
    fseeko(g_file, 0, SEEK_END);
    const std::uint64_t size = static_cast<std::uint64_t>(ftello(g_file));
    Decoder d;
    const auto t0 = std::chrono::steady_clock::now();
    d.start(read_file, nullptr, size, true);
    std::vector<std::uint32_t> pic;
    std::vector<std::int16_t> wav;
    int w = 0, h = 0, n = 0;
    while (d.next(pic, w, h, 5000)) {
        // 1920 sound frames per picture at 25 pictures a second
        std::vector<std::int16_t> chunk(1920 * 2, 0);
        d.mix(chunk.data(), 1920);
        wav.insert(wav.end(), chunk.begin(), chunk.end());
        if (n % 250 == 0) {
            const std::string path = std::string(argv[2]) + "/frame" + std::to_string(n) + ".ppm";
            std::FILE *f = std::fopen(path.c_str(), "wb");
            std::fprintf(f, "P6 %d %d 255\n", w, h);
            for (std::uint32_t px : pic) {
                const unsigned char rgb[3] = {static_cast<unsigned char>(px), static_cast<unsigned char>(px >> 8), static_cast<unsigned char>(px >> 16)};
                std::fwrite(rgb, 1, 3, f);
            }
            std::fclose(f);
        }
        ++n;
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%d pictures %dx%d in %.2f s (%.0f/s, %.1fx real time at 25/s)\n", n, w, h, s, n / s, n / 25.0 / s);
    const std::string path = std::string(argv[2]) + "/sound.wav";
    std::FILE *f = std::fopen(path.c_str(), "wb");
    const std::uint32_t bytes = static_cast<std::uint32_t>(wav.size() * 2), rate = kRate, brate = kRate * 4;
    std::fwrite("RIFF", 1, 4, f);
    const std::uint32_t riff = 36 + bytes;
    std::fwrite(&riff, 4, 1, f);
    std::fwrite("WAVEfmt ", 1, 8, f);
    const std::uint32_t fmt = 16;
    const std::uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    std::fwrite(&fmt, 4, 1, f);
    std::fwrite(&pcm, 2, 1, f);
    std::fwrite(&ch, 2, 1, f);
    std::fwrite(&rate, 4, 1, f);
    std::fwrite(&brate, 4, 1, f);
    std::fwrite(&align, 2, 1, f);
    std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f);
    std::fwrite(&bytes, 4, 1, f);
    std::fwrite(wav.data(), 2, wav.size(), f);
    std::fclose(f);
    d.stop();
    return 0;
}
#endif
