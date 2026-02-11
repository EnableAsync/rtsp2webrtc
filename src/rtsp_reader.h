#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

// Callback: receives NAL units (without start code), codec_id, keyframe flag, and PTS (90kHz)
using NalCallback = std::function<void(const uint8_t *data, size_t size,
                                       AVCodecID codec_id, bool is_keyframe,
                                       int64_t pts)>;

class RTSPReader {
public:
    RTSPReader(const std::string &url);
    ~RTSPReader();

    // Set callback before start()
    void setNalCallback(NalCallback cb) { nal_cb_ = std::move(cb); }

    // Get SPS/PPS extradata (available after start, once first packet arrives)
    const std::vector<uint8_t> &extradata() const { return extradata_; }
    AVCodecID codecId() const { return codec_id_; }

    void start();
    void stop();
    bool running() const { return running_; }

private:
    void readLoop();
    void parseAnnexB(const uint8_t *data, size_t size, bool is_keyframe);

    std::string url_;
    AVFormatContext *fmt_ctx_ = nullptr;
    int video_stream_idx_ = -1;
    AVCodecID codec_id_ = AV_CODEC_ID_NONE;
    std::vector<uint8_t> extradata_;

    NalCallback nal_cb_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};
