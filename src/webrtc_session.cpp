#include "webrtc_session.h"
#include <cstring>
#include <future>
#include <iostream>

WebRTCSession::WebRTCSession() {}

WebRTCSession::~WebRTCSession() {
    if (pc_)
        pc_->close();
}

std::string WebRTCSession::handleOffer(const std::string &sdp_offer) {
    rtc::Configuration config;
    config.iceServers.emplace_back("stun:stun.l.google.com:19302");

    pc_ = std::make_shared<rtc::PeerConnection>(config);

    // Create H.264 track
    rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
    media.addH264Codec(96);
    media.setBitrate(4000); // kbps

    track_ = pc_->addTrack(media);

    // RTP config: SSRC, cname, payloadType, clockRate
    auto rtp = std::make_shared<rtc::RtpPacketizationConfig>(
        42,                          // SSRC
        "rtsp2webrtc",               // cname
        96,                          // payloadType
        rtc::H264RtpPacketizer::defaultClockRate
    );
    rtp_config_ = rtp;

    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::LongStartSequence, rtp);

    track_->setMediaHandler(packetizer);

    // Set remote description (offer)
    pc_->setRemoteDescription(rtc::Description(sdp_offer, "offer"));

    // Wait for local description
    std::promise<std::string> answer_promise;
    auto answer_future = answer_promise.get_future();

    pc_->onLocalDescription([&answer_promise](rtc::Description desc) {
        answer_promise.set_value(std::string(desc));
    });

    pc_->onStateChange([](rtc::PeerConnection::State state) {
        std::cout << "[WebRTC] State: " << static_cast<int>(state) << "\n";
    });

    pc_->onGatheringStateChange(
        [](rtc::PeerConnection::GatheringState state) {
            std::cout << "[WebRTC] Gathering: " << static_cast<int>(state)
                      << "\n";
        });

    // Wait up to 10s for answer
    if (answer_future.wait_for(std::chrono::seconds(10)) ==
        std::future_status::timeout) {
        throw std::runtime_error("Timeout waiting for SDP answer");
    }

    return answer_future.get();
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
