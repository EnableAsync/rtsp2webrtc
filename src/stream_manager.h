#pragma once
#include "rtsp_reader.h"
#include "transcoder.h"
#include "webrtc_session.h"
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct StreamSource {
    std::unique_ptr<RTSPReader> reader;
    std::unique_ptr<Transcoder> transcoder; // non-null if H.265
    std::vector<std::shared_ptr<WebRTCSession>> sessions;
    std::mutex sessions_mtx;
};

class StreamManager {
public:
    StreamManager();
    ~StreamManager();

    void setPublicIP(const std::string &ip) { public_ip_ = ip; }

    // Create a new WebRTC session for the given RTSP URL
    // Returns SDP answer
    std::string createSession(const std::string &rtsp_url,
                              const std::string &sdp_offer);

    // Cleanup dead sessions periodically
    void cleanup();

private:
    StreamSource &getOrCreateSource(const std::string &rtsp_url);

    std::unordered_map<std::string, std::unique_ptr<StreamSource>> sources_;
    std::mutex sources_mtx_;
    std::string public_ip_;
};
