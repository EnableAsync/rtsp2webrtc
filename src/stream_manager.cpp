#include "stream_manager.h"
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <thread>

StreamManager::StreamManager() {}

StreamManager::~StreamManager() {
    std::lock_guard<std::mutex> lock(sources_mtx_);
    for (auto &[url, src] : sources_) {
        src->reader->stop();
    }
}

std::string StreamManager::createSession(const std::string &rtsp_url,
                                          const std::string &sdp_offer) {
    // Create source first to obtain stream info (SPS/PPS)
    StreamSource &source = getOrCreateSource(rtsp_url);

    // Wait for extradata (SPS/PPS) — available once RTSP stream opens
    std::string profile;
    for (int i = 0; i < 50; i++) {
        const auto &extra = source.reader->extradata();
        if (!extra.empty()) {
            // Find SPS NAL (type 7) in Annex-B, extract profile-level-id
            for (size_t j = 0; j + 7 < extra.size(); j++) {
                if (extra[j]==0 && extra[j+1]==0 && extra[j+2]==0 && extra[j+3]==1
                    && (extra[j+4] & 0x1F) == 7) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "%02x%02x%02x",
                             extra[j+5], extra[j+6], extra[j+7]);
                    profile = buf;
                    std::cout << "[SPS] profile-level-id=" << profile << "\n";
                    break;
                }
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto session = std::make_shared<WebRTCSession>();
    std::string answer = session->handleOffer(sdp_offer, public_ip_, profile);

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
                                sess->sendFrame(h264_data, h264_size, kf);
                            }
                        });
                }
                // Data is already Annex-B, feed directly to transcoder
                src_ptr->transcoder->feed(data, size, 0, 0);
            } else {
                // H.264 — direct pass-through (whole Annex-B frame)
                if (is_keyframe) {
                    std::string types;
                    for (size_t i = 0; i+4 < size; i++)
                        if (data[i]==0 && data[i+1]==0 && data[i+2]==0 && data[i+3]==1)
                            types += std::to_string(data[i+4] & 0x1F) + " ";
                    std::cout << "[H264] Keyframe NALs: " << types << "size=" << size << "\n";
                }

                const auto &extra = src_ptr->reader->extradata();
                if (is_keyframe && !extra.empty()) {
                    std::vector<uint8_t> buf(extra.size() + size);
                    memcpy(buf.data(), extra.data(), extra.size());
                    memcpy(buf.data() + extra.size(), data, size);
                    std::lock_guard<std::mutex> lock(src_ptr->sessions_mtx);
                    for (auto &sess : src_ptr->sessions)
                        sess->sendFrame(buf.data(), buf.size(), true);
                } else {
                    std::lock_guard<std::mutex> lock(src_ptr->sessions_mtx);
                    for (auto &sess : src_ptr->sessions)
                        sess->sendFrame(data, size, is_keyframe);
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
