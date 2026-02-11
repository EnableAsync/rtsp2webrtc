#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rtc/rtc.hpp>

class WebRTCSession {
public:
    WebRTCSession();
    ~WebRTCSession();

    // Process SDP offer, return SDP answer
    std::string handleOffer(const std::string &sdp_offer);

    // Send H.264 NAL unit (without Annex-B start code)
    void sendNal(const uint8_t *data, size_t size, bool is_keyframe);

    bool isOpen() const;
    std::string id() const;

private:
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::Track> track_;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtp_config_;
    uint32_t timestamp_ = 0;
    std::mutex send_mtx_;
};
