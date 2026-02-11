#include "webrtc_session.h"
#include <cstring>
#include <future>
#include <iostream>
#include <sstream>

WebRTCSession::WebRTCSession() {}

WebRTCSession::~WebRTCSession() {
    if (pc_)
        pc_->close();
}

std::string WebRTCSession::handleOffer(const std::string &sdp_offer,
                                       const std::string &public_ip,
                                       const std::string &profile_level_id) {
    rtc::Configuration config;
    config.iceServers.emplace_back("stun:stun.l.google.com:19302");

    // Fixed port for SSH forwarding scenarios
    config.portRangeBegin = 9000;
    config.portRangeEnd = 9000;
    config.enableIceTcp = true;

    pc_ = std::make_shared<rtc::PeerConnection>(config);

    // Parse offer to find video section's mid
    rtc::Description parsed_offer(sdp_offer, "offer");
    std::string video_mid = "0"; // fallback
    for (int i = 0; i < parsed_offer.mediaCount(); ++i) {
        auto entry = parsed_offer.media(i);
        if (auto *media_ptr = std::get_if<rtc::Description::Media *>(&entry)) {
            if ((*media_ptr)->type() == "video") {
                video_mid = (*media_ptr)->mid();
                break;
            }
        }
    }

    // Find H264 PT with packetization-mode=1 from offer, prefer High profile
    int h264_pt = 96;
    std::string h264_fmtp;
    {
        std::istringstream iss(sdp_offer);
        std::string line;
        std::vector<int> pts;
        while (std::getline(iss, line)) {
            if (line.find("a=rtpmap:") != std::string::npos &&
                line.find("H264/90000") != std::string::npos) {
                pts.push_back(std::stoi(line.substr(line.find(':') + 1)));
            }
        }
        int best_pt = -1;
        std::string best_fmtp;
        for (int pt : pts) {
            std::string prefix = "a=fmtp:" + std::to_string(pt) + " ";
            iss.clear(); iss.str(sdp_offer);
            while (std::getline(iss, line)) {
                if (line.rfind(prefix, 0) == 0 &&
                    line.find("packetization-mode=1") != std::string::npos) {
                    std::string fmtp = line.substr(prefix.size());
                    if (fmtp.back() == '\r') fmtp.pop_back();
                    // Prefer High profile (64xxxx)
                    if (fmtp.find("profile-level-id=64") != std::string::npos) {
                        best_pt = pt;
                        best_fmtp = fmtp;
                    } else if (best_pt < 0) {
                        best_pt = pt;
                        best_fmtp = fmtp;
                    }
                    break;
                }
            }
        }
        if (best_pt > 0) {
            h264_pt = best_pt;
            h264_fmtp = best_fmtp;
        }
    }
    std::cout << "[WebRTC] H264 PT=" << h264_pt << " fmtp=" << h264_fmtp << "\n";

    // Create H.264 track with matching mid and PT from offer
    rtc::Description::Video media(video_mid, rtc::Description::Direction::SendOnly);
    if (!h264_fmtp.empty())
        media.addH264Codec(h264_pt, h264_fmtp);
    else
        media.addH264Codec(h264_pt);
    media.setBitrate(4000); // kbps
    media.addSSRC(42, "rtsp2webrtc", "stream0", "video0");

    track_ = pc_->addTrack(media);

    // RTP config: SSRC, cname, payloadType, clockRate
    auto rtp = std::make_shared<rtc::RtpPacketizationConfig>(
        42,                          // SSRC
        "rtsp2webrtc",               // cname
        h264_pt,                     // payloadType from offer
        rtc::H264RtpPacketizer::defaultClockRate
    );
    rtp_config_ = rtp;

    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::LongStartSequence, rtp);

    sr_reporter_ = std::make_shared<rtc::RtcpSrReporter>(rtp);
    packetizer->addToChain(sr_reporter_);

    auto nack_responder = std::make_shared<rtc::RtcpNackResponder>();
    sr_reporter_->addToChain(nack_responder);

    track_->setMediaHandler(packetizer);

    // Wait for ICE gathering to complete before returning answer
    std::promise<void> gathering_done;
    auto gathering_future = gathering_done.get_future();

    pc_->onStateChange([](rtc::PeerConnection::State state) {
        std::cout << "[WebRTC] State: " << static_cast<int>(state) << "\n";
    });

    pc_->onGatheringStateChange(
        [&gathering_done](rtc::PeerConnection::GatheringState state) {
            std::cout << "[WebRTC] Gathering: " << static_cast<int>(state)
                      << "\n";
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                gathering_done.set_value();
            }
        });

    // Set remote description (offer) — triggers answer generation + ICE gathering
    pc_->setRemoteDescription(rtc::Description(sdp_offer, "offer"));

    // Wait for ICE gathering to complete (up to 10s)
    if (gathering_future.wait_for(std::chrono::seconds(10)) ==
        std::future_status::timeout) {
        throw std::runtime_error("Timeout waiting for ICE gathering");
    }

    // Now local description contains all ICE candidates
    auto desc = pc_->localDescription();

    // If public_ip is set (e.g. FRP/NAT), add it as a high-priority candidate
    if (!public_ip.empty()) {
        std::string candidate_str =
            "candidate:100 1 UDP 2130706431 " + public_ip +
            " 9000 typ host";
        desc->addCandidate(rtc::Candidate(candidate_str, video_mid));
    }

    std::string answer_sdp = std::string(*desc);
    std::cout << "[WebRTC] Answer SDP:\n" << answer_sdp << "\n";
    return answer_sdp;
}

void WebRTCSession::sendFrame(const uint8_t *data, size_t size,
                               bool is_keyframe) {
    std::lock_guard<std::mutex> lock(send_mtx_);
    if (!track_ || !track_->isOpen())
        return;

    // Wait for keyframe before sending (browser decoder needs it)
    if (!got_keyframe_) {
        if (!is_keyframe)
            return;
        got_keyframe_ = true;
        std::cout << "[WebRTC] First keyframe, starting send\n";
    }

    // One timestamp per frame (3000 = 90kHz / 30fps)
    timestamp_ += 3000;
    rtp_config_->timestamp = timestamp_;

    // Data is already Annex-B (start codes included)
    // H264RtpPacketizer will split NALs and use same timestamp
    std::vector<std::byte> frame(size);
    std::memcpy(frame.data(), data, size);

    try {
        sr_reporter_->setNeedsToReport();
        bool ok = track_->send(frame);
        frame_count_++;
        if (frame_count_ <= 3 || frame_count_ % 100 == 0)
            std::cout << "[WebRTC] send #" << frame_count_
                      << " size=" << size << " ts=" << timestamp_
                      << " kf=" << is_keyframe << " ok=" << ok << "\n";
    } catch (const std::exception &e) {
        std::cerr << "[WebRTC] Send error: " << e.what() << "\n";
    }
}

void WebRTCSession::sendNal(const uint8_t *data, size_t size,
                             bool is_keyframe) {
    std::lock_guard<std::mutex> lock(send_mtx_);
    if (!track_ || !track_->isOpen())
        return;

    // Increment timestamp (3000 = 90kHz / 30fps)
    timestamp_ += 3000;

    rtp_config_->timestamp = timestamp_;

    // Send as Annex-B with start code (packetizer expects it)
    std::vector<std::byte> nal_with_sc(4 + size);
    nal_with_sc[0] = std::byte{0x00};
    nal_with_sc[1] = std::byte{0x00};
    nal_with_sc[2] = std::byte{0x00};
    nal_with_sc[3] = std::byte{0x01};
    std::memcpy(nal_with_sc.data() + 4, data, size);

    try {
        track_->send(nal_with_sc);
    } catch (const std::exception &e) {
        std::cerr << "[WebRTC] Send error: " << e.what() << "\n";
    }
}

bool WebRTCSession::isOpen() const {
    return pc_ && pc_->state() == rtc::PeerConnection::State::Connected;
}

std::string WebRTCSession::id() const {
    if (pc_)
        return pc_->localDescription()->typeString();
    return "";
}
