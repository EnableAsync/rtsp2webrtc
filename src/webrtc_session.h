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
    // public_ip: optional external IP for ICE candidates (e.g. FRP server)
    std::string handleOffer(const std::string &sdp_offer,
                            const std::string &public_ip = "",
                            const std::string &profile_level_id = "");

    // Send H.264 Annex-B frame (one or more NALs with start codes)
    // pts: 90kHz timestamp from RTSP, or -1 for auto-increment
    void sendNal(const uint8_t *data, size_t size, bool is_keyframe);
    void sendFrame(const uint8_t *data, size_t size, bool is_keyframe,
                   int64_t pts = -1);

    bool isOpen() const;
    std::string id() const;

private:
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::Track> track_;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtp_config_;
    std::shared_ptr<rtc::RtcpSrReporter> sr_reporter_;
    uint32_t timestamp_ = 0;
    int64_t first_pts_ = -1;
    uint64_t frame_count_ = 0;
    bool got_keyframe_ = false;
    std::mutex send_mtx_;
    int64_t last_rtsp_pts_ = -1;
};
