// SPDX-License-Identifier: GPL-3.0-or-later
#include "decoder.hpp"
#include "../log.hpp"
#include "../net.hpp"
#include <cstdio>
#include <cstring>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace audio {

struct Decoder::Impl {
    std::string url;
    net::Stream http;
    std::FILE* file = nullptr; // a local song instead of the phone's stream
    std::uint64_t offset = 0, size = 0; // byte position in the file, total size
    AVFormatContext* fmt = nullptr;
    AVIOContext* io = nullptr;
    AVCodecContext* codec = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    int stream = -1;
    std::vector<std::int16_t> pending; // converted frames not yet handed out
    std::size_t pending_at = 0;
    long long next_ms = 0, skip_frames = 0, frames_out = 0; // position = next_ms + frames_out / rate
    bool eof = false;

    static int read_cb(void* opaque, std::uint8_t* buf, int size) {
        auto* d = static_cast<Impl*>(opaque);
        if (d->file) {
            const std::size_t n = std::fread(buf, 1, std::size_t(size), d->file);
            d->offset += n;
            return n > 0 ? int(n) : std::ferror(d->file) ? AVERROR(EIO) : AVERROR_EOF;
        }
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (d->http.fd < 0 && !d->http.open(d->url, d->offset)) continue;
            const long n = d->http.read(buf, std::size_t(size));
            if (n > 0) { d->offset += std::uint64_t(n); return int(n); }
            if (n == 0 && d->size && d->offset >= d->size) return AVERROR_EOF;
            d->http.close(); // dropped mid-song: reconnect at the same byte
        }
        return n_error();
    }
    static int n_error() { return AVERROR(EIO); }
    static std::int64_t seek_cb(void* opaque, std::int64_t pos, int whence) {
        auto* d = static_cast<Impl*>(opaque);
        if (whence & AVSEEK_SIZE) return d->size ? std::int64_t(d->size) : -1;
        whence &= ~AVSEEK_FORCE;
        std::int64_t to = whence == SEEK_SET ? pos : whence == SEEK_CUR ? std::int64_t(d->offset) + pos : whence == SEEK_END && d->size ? std::int64_t(d->size) + pos : -1;
        if (to < 0) return -1;
        if (d->file) {
            if (fseeko(d->file, off_t(to), SEEK_SET)) return -1;
        } else if (std::uint64_t(to) != d->offset) d->http.close(); // reopened lazily with Range
        d->offset = std::uint64_t(to);
        return to;
    }
    void reset() {
        if (swr) swr_free(&swr);
        if (codec) avcodec_free_context(&codec);
        if (fmt) avformat_close_input(&fmt);
        if (io) { av_freep(&io->buffer); avio_context_free(&io); }
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
        http.close();
        if (file) { std::fclose(file); file = nullptr; }
        pending.clear();
        pending_at = 0;
        offset = size = 0;
        stream = -1;
        eof = false;
        skip_frames = frames_out = 0;
    }
};

Decoder::Decoder() : d(new Impl) { av_log_set_level(AV_LOG_ERROR); }
Decoder::~Decoder() { close(); }
void Decoder::close() { d->reset(); }

bool Decoder::open(const std::string& url, long long start_ms) {
    close();
    d->url = url;
    if (!url.empty() && url[0] == '/') { // a file on the console
        if (!(d->file = std::fopen(url.c_str(), "rb"))) { logf("DECODER can't open file"); return false; }
        fseeko(d->file, 0, SEEK_END);
        d->size = std::uint64_t(ftello(d->file));
        fseeko(d->file, 0, SEEK_SET);
    } else if (!d->http.open(url, 0)) { logf("DECODER http open failed"); return false; }
    else d->size = d->http.head.total;
    const int buffer_size = 64 * 1024;
    auto* buffer = static_cast<unsigned char*>(av_malloc(buffer_size));
    d->io = avio_alloc_context(buffer, buffer_size, 0, d.get(), Impl::read_cb, nullptr, Impl::seek_cb);
    d->fmt = avformat_alloc_context();
    d->fmt->pb = d->io;
    d->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    int rc = avformat_open_input(&d->fmt, nullptr, nullptr, nullptr);
    if (rc < 0 || (rc = avformat_find_stream_info(d->fmt, nullptr)) < 0) { logf("DECODER open rc=%d", rc); close(); return false; }
    const AVCodec* c = nullptr;
    d->stream = av_find_best_stream(d->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &c, 0);
    if (d->stream < 0 || !c) { logf("DECODER no audio stream"); close(); return false; }
    d->codec = avcodec_alloc_context3(c);
    avcodec_parameters_to_context(d->codec, d->fmt->streams[d->stream]->codecpar);
    if ((rc = avcodec_open2(d->codec, c, nullptr)) < 0) { logf("DECODER codec %s rc=%d", c->name, rc); close(); return false; }
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&d->swr, &stereo, AV_SAMPLE_FMT_S16, rate, &d->codec->ch_layout, d->codec->sample_fmt,
                            d->codec->sample_rate, 0, nullptr) < 0 || swr_init(d->swr) < 0) {
        logf("DECODER resampler failed");
        close();
        return false;
    }
    d->packet = av_packet_alloc();
    d->frame = av_frame_alloc();
    logf("DECODER %s %d Hz %d ch, %llu bytes", c->name, d->codec->sample_rate, d->codec->ch_layout.nb_channels, (unsigned long long)d->size);
    d->next_ms = d->frames_out = 0;
    return start_ms <= 0 || seek(start_ms);
}

bool Decoder::seek(long long ms) {
    if (!d->fmt) return false;
    const AVStream* s = d->fmt->streams[d->stream];
    const std::int64_t ts = av_rescale_q(ms, AVRational{1, 1000}, s->time_base);
    if (av_seek_frame(d->fmt, d->stream, ts, AVSEEK_FLAG_BACKWARD) < 0) return false;
    avcodec_flush_buffers(d->codec);
    swr_init(d->swr); // drop resampler history
    d->pending.clear();
    d->pending_at = 0;
    d->eof = false;
    d->next_ms = ms;
    d->frames_out = 0;
    d->skip_frames = -1; // set from the first decoded frame's timestamp: land exactly on `ms`
    return true;
}

int Decoder::read(std::int16_t* out, int frames) {
    int got = 0;
    while (got < frames) {
        if (d->pending_at < d->pending.size()) {
            const std::size_t n = std::min<std::size_t>(std::size_t(frames - got), (d->pending.size() - d->pending_at) / 2);
            std::memcpy(out + 2 * got, d->pending.data() + d->pending_at, n * 4);
            d->pending_at += n * 2;
            got += int(n);
            continue;
        }
        if (d->eof || !d->fmt) break;
        int rc = avcodec_receive_frame(d->codec, d->frame);
        if (rc == AVERROR(EAGAIN)) {
            rc = av_read_frame(d->fmt, d->packet);
            if (rc == AVERROR_EOF) { avcodec_send_packet(d->codec, nullptr); continue; }
            if (rc < 0) { logf("DECODER read rc=%d", rc); return got ? got : -1; }
            if (d->packet->stream_index == d->stream) avcodec_send_packet(d->codec, d->packet);
            av_packet_unref(d->packet);
            continue;
        }
        if (rc == AVERROR_EOF) { d->eof = true; break; }
        if (rc < 0) { logf("DECODER decode rc=%d", rc); return got ? got : -1; }
        const int cap = int(swr_get_out_samples(d->swr, d->frame->nb_samples));
        d->pending.resize(std::size_t(cap) * 2);
        auto* dst = reinterpret_cast<std::uint8_t*>(d->pending.data());
        const int n = swr_convert(d->swr, &dst, cap, const_cast<const std::uint8_t**>(d->frame->extended_data), d->frame->nb_samples);
        d->pending.resize(std::size_t(n > 0 ? n : 0) * 2);
        d->pending_at = 0;
        if (d->skip_frames < 0) {
            // After a seek the demuxer lands on a cue/keyframe that can be seconds early: drop
            // everything before the target, across as many frames as it takes.
            const AVStream* s = d->fmt->streams[d->stream];
            const std::int64_t ts = d->frame->best_effort_timestamp;
            if (ts == AV_NOPTS_VALUE) d->skip_frames = 0; // no timing: trust the seek
            else {
                const long long pts = av_rescale_q(ts, s->time_base, AVRational{1, 1000});
                const long long drop = pts < d->next_ms ? (d->next_ms - pts) * rate / 1000 : 0;
                d->pending_at = std::min<std::size_t>(d->pending.size(), std::size_t(drop) * 2);
                if (d->pending_at < d->pending.size()) d->skip_frames = 0; // reached the target
            }
        }
        av_frame_unref(d->frame);
    }
    d->frames_out += got;
    return got;
}

long long Decoder::position_ms() const { return d->next_ms + d->frames_out * 1000 / rate; }
long long Decoder::duration_ms() const {
    return d->fmt && d->fmt->duration > 0 ? d->fmt->duration / (AV_TIME_BASE / 1000) : -1;
}
}
