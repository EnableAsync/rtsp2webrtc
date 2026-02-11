#pragma once
#include <cstdint>
#include <functional>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

// Transcodes H.265 NAL units to H.264
class Transcoder {
public:
    using OutputCallback =
        std::function<void(const uint8_t *data, size_t size, bool is_keyframe)>;

    Transcoder();
    ~Transcoder();

    bool init(const AVCodecParameters *hevc_params);
    void setOutputCallback(OutputCallback cb) { output_cb_ = std::move(cb); }

    // Feed H.265 packet (raw Annex-B with start codes)
    void feed(const uint8_t *data, size_t size, int64_t pts, int64_t dts);

private:
    AVCodecContext *dec_ctx_ = nullptr;
    AVCodecContext *enc_ctx_ = nullptr;
    SwsContext *sws_ctx_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVFrame *sw_frame_ = nullptr; // for pixel format conversion
    AVPacket *enc_pkt_ = nullptr;
    OutputCallback output_cb_;
    bool initialized_ = false;
};
