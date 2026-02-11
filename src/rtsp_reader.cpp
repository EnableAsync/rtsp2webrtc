#include "rtsp_reader.h"
#include <cstring>
#include <iostream>

extern "C" {
#include <libavutil/error.h>
}

RTSPReader::RTSPReader(const std::string &url) : url_(url) {}

RTSPReader::~RTSPReader() { stop(); }

void RTSPReader::start() {
    if (running_)
        return;
    running_ = true;
    thread_ = std::thread(&RTSPReader::readLoop, this);
}

void RTSPReader::stop() {
    running_ = false;
    if (thread_.joinable())
        thread_.join();
}

void RTSPReader::readLoop() {
    // Open RTSP
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "stimeout", "5000000", 0); // 5s timeout

    int ret = avformat_open_input(&fmt_ctx_, url_.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        std::cerr << "[RTSPReader] Failed to open " << url_ << ": " << err << "\n";
        running_ = false;
        return;
    }

    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
        std::cerr << "[RTSPReader] Failed to find stream info\n";
        avformat_close_input(&fmt_ctx_);
        running_ = false;
        return;
    }

    // Find video stream
    for (unsigned i = 0; i < fmt_ctx_->nb_streams; i++) {
        if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx_ = i;
            codec_id_ = fmt_ctx_->streams[i]->codecpar->codec_id;
            // Copy extradata (SPS/PPS)
            auto *par = fmt_ctx_->streams[i]->codecpar;
            if (par->extradata && par->extradata_size > 0) {
                extradata_.assign(par->extradata,
                                  par->extradata + par->extradata_size);
            }
            break;
        }
    }

    if (video_stream_idx_ < 0) {
        std::cerr << "[RTSPReader] No video stream found\n";
        avformat_close_input(&fmt_ctx_);
        running_ = false;
        return;
    }

    std::cout << "[RTSPReader] Stream opened: "
              << avcodec_get_name(codec_id_) << "\n";

    // Read loop
    AVPacket *pkt = av_packet_alloc();
    while (running_) {
        ret = av_read_frame(fmt_ctx_, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF)
                std::cout << "[RTSPReader] EOF\n";
            else
                std::cerr << "[RTSPReader] Read error\n";
            break;
        }
        if (pkt->stream_index == video_stream_idx_ && nal_cb_) {
            bool is_keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            // Pass entire Annex-B frame (all NALs with start codes)
            nal_cb_(pkt->data, pkt->size, codec_id_, is_keyframe);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fmt_ctx_);
    running_ = false;
}

// Parse Annex-B byte stream, split into individual NAL units
void RTSPReader::parseAnnexB(const uint8_t *data, size_t size,
                              bool is_keyframe) {
    size_t i = 0;
    while (i < size) {
        // Find start code (0x000001 or 0x00000001)
        size_t sc_len = 0;
        if (i + 3 <= size && data[i] == 0 && data[i + 1] == 0 &&
            data[i + 2] == 1) {
            sc_len = 3;
        } else if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 &&
                   data[i + 2] == 0 && data[i + 3] == 1) {
            sc_len = 4;
        } else {
            i++;
            continue;
        }

        size_t nal_start = i + sc_len;

        // Find next start code
        size_t nal_end = size;
        for (size_t j = nal_start + 1; j + 2 < size; j++) {
            if (data[j] == 0 && data[j + 1] == 0 &&
                (data[j + 2] == 1 ||
                 (j + 3 < size && data[j + 2] == 0 && data[j + 3] == 1))) {
                nal_end = j;
                break;
            }
        }

        if (nal_end > nal_start) {
            nal_cb_(data + nal_start, nal_end - nal_start, codec_id_,
                    is_keyframe);
        }
        i = nal_end;
    }
}
