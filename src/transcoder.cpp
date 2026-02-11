#include "transcoder.h"
#include <iostream>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

Transcoder::Transcoder() {
    frame_ = av_frame_alloc();
    enc_pkt_ = av_packet_alloc();
}

Transcoder::~Transcoder() {
    av_frame_free(&frame_);
    av_frame_free(&sw_frame_);
    av_packet_free(&enc_pkt_);
    if (dec_ctx_)
        avcodec_free_context(&dec_ctx_);
    if (enc_ctx_)
        avcodec_free_context(&enc_ctx_);
    if (sws_ctx_)
        sws_freeContext(sws_ctx_);
}

bool Transcoder::init(const AVCodecParameters *hevc_params) {
    const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!decoder) {
        std::cerr << "[Transcoder] HEVC decoder not found\n";
        return false;
    }
    dec_ctx_ = avcodec_alloc_context3(decoder);
    if (hevc_params)
        avcodec_parameters_to_context(dec_ctx_, hevc_params);
    if (avcodec_open2(dec_ctx_, decoder, nullptr) < 0) {
        std::cerr << "[Transcoder] Failed to open HEVC decoder\n";
        return false;
    }

    initialized_ = true;
    return true;
}

void Transcoder::feed(const uint8_t *data, size_t size, int64_t pts,
                      int64_t dts) {
    if (!initialized_)
        return;

    AVPacket *pkt = av_packet_alloc();
    pkt->data = const_cast<uint8_t *>(data);
    pkt->size = static_cast<int>(size);
    pkt->pts = pts;
    pkt->dts = dts;

    int ret = avcodec_send_packet(dec_ctx_, pkt);
    av_packet_free(&pkt);
    if (ret < 0)
        return;

    while (true) {
        ret = avcodec_receive_frame(dec_ctx_, frame_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
            break;

        // Lazy-init encoder + swscale on first decoded frame
        if (!enc_ctx_) {
            const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
            if (!encoder) {
                std::cerr << "[Transcoder] H.264 encoder not found\n";
                av_frame_unref(frame_);
                return;
            }
            enc_ctx_ = avcodec_alloc_context3(encoder);
            enc_ctx_->width = frame_->width;
            enc_ctx_->height = frame_->height;
            enc_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
            enc_ctx_->time_base = {1, 30};
            enc_ctx_->framerate = {30, 1};
            enc_ctx_->gop_size = 60;
            enc_ctx_->max_b_frames = 0;

            av_opt_set(enc_ctx_->priv_data, "preset", "ultrafast", 0);
            av_opt_set(enc_ctx_->priv_data, "tune", "zerolatency", 0);
            av_opt_set(enc_ctx_->priv_data, "profile", "baseline", 0);

            if (avcodec_open2(enc_ctx_, encoder, nullptr) < 0) {
                std::cerr << "[Transcoder] Failed to open H.264 encoder\n";
                avcodec_free_context(&enc_ctx_);
                av_frame_unref(frame_);
                return;
            }

            // Setup swscale if needed
            auto src_fmt = static_cast<AVPixelFormat>(frame_->format);
            if (src_fmt != AV_PIX_FMT_YUV420P) {
                sws_ctx_ = sws_getContext(
                    frame_->width, frame_->height, src_fmt, frame_->width,
                    frame_->height, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR,
                    nullptr, nullptr, nullptr);
                sw_frame_ = av_frame_alloc();
                sw_frame_->format = AV_PIX_FMT_YUV420P;
                sw_frame_->width = frame_->width;
                sw_frame_->height = frame_->height;
                av_frame_get_buffer(sw_frame_, 0);
            }

            std::cout << "[Transcoder] Initialized: " << frame_->width << "x"
                      << frame_->height << " fmt=" << frame_->format << "\n";
        }

        // Convert pixel format if needed
        AVFrame *enc_frame = frame_;
        if (sws_ctx_ && sw_frame_) {
            sws_scale(sws_ctx_, frame_->data, frame_->linesize, 0,
                      frame_->height, sw_frame_->data, sw_frame_->linesize);
            sw_frame_->pts = frame_->pts;
            enc_frame = sw_frame_;
        }

        // Encode
        ret = avcodec_send_frame(enc_ctx_, enc_frame);
        if (ret < 0) {
            av_frame_unref(frame_);
            continue;
        }

        while (true) {
            ret = avcodec_receive_packet(enc_ctx_, enc_pkt_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                break;

            if (output_cb_) {
                bool kf = (enc_pkt_->flags & AV_PKT_FLAG_KEY) != 0;
                output_cb_(enc_pkt_->data, enc_pkt_->size, kf);
            }
            av_packet_unref(enc_pkt_);
        }
        av_frame_unref(frame_);
    }
}
