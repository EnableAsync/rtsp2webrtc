#include "stream_manager.h"
#include <algorithm>
#include <iostream>

StreamManager::StreamManager() {}

StreamManager::~StreamManager() {
    std::lock_guard<std::mutex> lock(sources_mtx_);
    for (auto &[url, src] : sources_) {
        src->reader->stop();
    }
}

std::string StreamManager::createSession(const std::string &rtsp_url,
                                          const std::string &sdp_offer) {
    auto session = std::make_shared<WebRTCSession>();
    std::string answer = session->handleOffer(sdp_offer);

    StreamSource &source = getOrCreateSource(rtsp_url);
    {
        std::lock_guard<std::mutex> lock(source.sessions_mtx);
        source.sessions.push_back(session);
    }

    return answer;
}

StreamSource &StreamManager::getOrCreateSource(const std::string &rtsp_url) {
    std::lock_guard<std::mutex> lock(sources_mtx_);

    auto it = sources_.find(rtsp_url);
    if (it != sources_.end())
        return *it->second;

    auto src = std::make_unique<StreamSource>();
    src->reader = std::make_unique<RTSPReader>(rtsp_url);

    // Set NAL callback — dispatches to all sessions
    StreamSource *src_ptr = src.get();
    src->reader->setNalCallback(
        [src_ptr](const uint8_t *data, size_t size, AVCodecID codec_id,
                  bool is_keyframe) {
            if (codec_id == AV_CODEC_ID_HEVC) {
                // Need transcoding
                if (!src_ptr->transcoder) {
                    src_ptr->transcoder = std::make_unique<Transcoder>();
                    // Get codec params from reader for init
                    auto *reader = src_ptr->reader.get();
                    // Initialize with extradata from the stream
                    AVCodecParameters *params = avcodec_parameters_alloc();
                    params->codec_id = AV_CODEC_ID_HEVC;
                    params->codec_type = AVMEDIA_TYPE_VIDEO;
                    if (!reader->extradata().empty()) {
                        params->extradata = static_cast<uint8_t *>(
                            av_malloc(reader->extradata().size() +
                                      AV_INPUT_BUFFER_PADDING_SIZE));
                        memcpy(params->extradata, reader->extradata().data(),
                               reader->extradata().size());
                        params->extradata_size =
                            static_cast<int>(reader->extradata().size());
                    }
                    src_ptr->transcoder->init(params);
                    avcodec_parameters_free(&params);

                    // Set transcoder output → sessions
                    src_ptr->transcoder->setOutputCallback(
                        [src_ptr](const uint8_t *h264_data, size_t h264_size,
                                  bool kf) {
                            std::lock_guard<std::mutex> lock(
                                src_ptr->sessions_mtx);
                            for (auto &sess : src_ptr->sessions) {
                                sess->sendNal(h264_data, h264_size, kf);
                            }
                        });
                }
                // Feed raw packet with start codes for decoder
                // Reconstruct Annex-B with start code
                std::vector<uint8_t> annexb(4 + size);
                annexb[0] = 0;
                annexb[1] = 0;
                annexb[2] = 0;
                annexb[3] = 1;
                memcpy(annexb.data() + 4, data, size);
                src_ptr->transcoder->feed(annexb.data(), annexb.size(), 0, 0);
            } else {
                // H.264 — direct pass-through
                std::lock_guard<std::mutex> lock(src_ptr->sessions_mtx);
                for (auto &sess : src_ptr->sessions) {
                    sess->sendNal(data, size, is_keyframe);
                }
            }
        });

    src->reader->start();
    std::cout << "[StreamManager] Started source: " << rtsp_url << "\n";

    auto &ref = *src;
    sources_[rtsp_url] = std::move(src);
    return ref;
}

void StreamManager::cleanup() {
    std::lock_guard<std::mutex> lock(sources_mtx_);
    for (auto it = sources_.begin(); it != sources_.end();) {
        auto &src = it->second;
        {
            std::lock_guard<std::mutex> slock(src->sessions_mtx);
            // Remove dead sessions
            src->sessions.erase(
                std::remove_if(src->sessions.begin(), src->sessions.end(),
                               [](const auto &s) { return !s->isOpen(); }),
                src->sessions.end());
        }
        // If no sessions and reader stopped, remove source
        if (src->sessions.empty() && !src->reader->running()) {
            std::cout << "[StreamManager] Removing source: " << it->first
                      << "\n";
            it = sources_.erase(it);
        } else {
            ++it;
        }
    }
}
