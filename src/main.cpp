#include "stream_manager.h"
#include <httplib.h>
#include <iostream>
#include <nlohmann/json.hpp>

// Embed index.html as string
static const char *INDEX_HTML = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>RTSP to WebRTC Player</title>
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
body { font-family: -apple-system, BlinkMacSystemFont, sans-serif; background: #1a1a2e; color: #eee; min-height: 100vh; display: flex; flex-direction: column; align-items: center; padding: 20px; }
h1 { margin-bottom: 20px; color: #e94560; }
.controls { display: flex; gap: 10px; margin-bottom: 20px; width: 100%; max-width: 800px; }
input { flex: 1; padding: 10px 16px; border: 1px solid #333; border-radius: 8px; background: #16213e; color: #eee; font-size: 14px; }
button { padding: 10px 24px; border: none; border-radius: 8px; background: #e94560; color: #fff; font-size: 14px; cursor: pointer; transition: background 0.2s; }
button:hover { background: #c73e54; }
button:disabled { background: #555; cursor: not-allowed; }
video { width: 100%; max-width: 800px; background: #000; border-radius: 8px; }
#status { margin-top: 10px; color: #888; font-size: 13px; }
</style>
</head>
<body>
<h1>RTSP → WebRTC</h1>
<div class="controls">
    <input id="url" type="text" placeholder="rtsp://username:password@host:554/stream" value="">
    <button id="play" onclick="startPlay()">Play</button>
    <button id="stop" onclick="stopPlay()" disabled>Stop</button>
</div>
<video id="video" autoplay muted playsinline></video>
<div id="status">Ready</div>

<script>
let pc = null;

function setStatus(msg) {
    document.getElementById('status').textContent = msg;
}

async function startPlay() {
    const url = document.getElementById('url').value.trim();
    if (!url) { setStatus('Please enter RTSP URL'); return; }

    document.getElementById('play').disabled = true;
    document.getElementById('stop').disabled = false;
    setStatus('Connecting...');

    try {
        pc = new RTCPeerConnection({
            iceServers: [{ urls: 'stun:stun.l.google.com:19302' }]
        });

        pc.addTransceiver('video', { direction: 'recvonly' });

        pc.ontrack = (ev) => {
            console.log('[ontrack] kind=' + ev.track.kind + ' state=' + ev.track.readyState + ' streams=' + ev.streams.length);
            const stream = ev.streams[0] || new MediaStream([ev.track]);
            const video = document.getElementById('video');
            video.srcObject = stream;
            setStatus('Track received, waiting for frames...');

            ev.track.onmute = () => console.log('[track] muted');
            ev.track.onunmute = () => console.log('[track] unmuted');
            ev.track.onended = () => console.log('[track] ended');
        };

        const video = document.getElementById('video');
        video.onloadedmetadata = () => console.log('[video] loadedmetadata ' + video.videoWidth + 'x' + video.videoHeight);
        video.onplaying = () => { console.log('[video] playing'); setStatus('Playing'); };
        video.onstalled = () => console.log('[video] stalled');
        video.onerror = (e) => console.log('[video] error', video.error);
        video.onwaiting = () => console.log('[video] waiting');

        pc.oniceconnectionstatechange = () => {
            console.log('[ICE] ' + pc.iceConnectionState);
            setStatus('ICE: ' + pc.iceConnectionState);
            if (pc.iceConnectionState === 'disconnected' || pc.iceConnectionState === 'failed') {
                stopPlay();
            }
        };

        const offer = await pc.createOffer();
        await pc.setLocalDescription(offer);

        // Wait for ICE gathering
        await new Promise((resolve) => {
            if (pc.iceGatheringState === 'complete') resolve();
            else pc.onicegatheringstatechange = () => {
                if (pc.iceGatheringState === 'complete') resolve();
            };
        });

        console.log('[offer SDP]', pc.localDescription.sdp);

        const resp = await fetch('/api/offer', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                rtsp_url: url,
                sdp: pc.localDescription.sdp
            })
        });

        if (!resp.ok) throw new Error('Server error: ' + resp.status);
        const answer = await resp.json();
        await pc.setRemoteDescription(new RTCSessionDescription(answer));
        setStatus('Connected — checking stats...');

        // Periodic stats to check incoming RTP
        const statsTimer = setInterval(async () => {
            if (!pc) { clearInterval(statsTimer); return; }
            const stats = await pc.getStats();
            stats.forEach(s => {
                if (s.type === 'inbound-rtp' && s.kind === 'video') {
                    console.log('[stats] pkts=' + s.packetsReceived + ' bytes=' + s.bytesReceived + ' frames=' + (s.framesDecoded||0) + ' dropped=' + (s.framesDropped||0) + ' nacks=' + (s.nackCount||0) + ' pli=' + (s.pliCount||0) + ' decoder=' + (s.decoderImplementation||'none') + ' framesRcvd=' + (s.framesReceived||0) + ' codecId=' + (s.codecId||''));
                }
            });
        }, 2000);
    } catch (e) {
        setStatus('Error: ' + e.message);
        stopPlay();
    }
}

function stopPlay() {
    if (pc) { pc.close(); pc = null; }
    document.getElementById('video').srcObject = null;
    document.getElementById('play').disabled = false;
    document.getElementById('stop').disabled = true;
    setStatus('Stopped');
}
</script>
</body>
</html>
)HTML";

int main(int argc, char *argv[]) {
    int port = 8080;
    std::string public_ip;
    if (argc > 1)
        port = std::atoi(argv[1]);
    if (argc > 2)
        public_ip = argv[2];

    std::cout << "rtsp2webrtc starting on port " << port << "\n";
    if (!public_ip.empty())
        std::cout << "Public IP: " << public_ip << "\n";

    StreamManager manager;
    if (!public_ip.empty())
        manager.setPublicIP(public_ip);
    httplib::Server svr;

    // Serve web player
    svr.Get("/", [](const httplib::Request &, httplib::Response &res) {
        res.set_content(INDEX_HTML, "text/html");
    });

    // Signaling endpoint
    svr.Post("/api/offer",
             [&manager](const httplib::Request &req, httplib::Response &res) {
                 try {
                     auto j = nlohmann::json::parse(req.body);
                     std::string rtsp_url = j.at("rtsp_url");
                     std::string sdp = j.at("sdp");

                     std::cout << "[API] Offer for: " << rtsp_url << "\n";

                     std::string answer = manager.createSession(rtsp_url, sdp);

                     nlohmann::json resp;
                     resp["type"] = "answer";
                     resp["sdp"] = answer;
                     res.set_content(resp.dump(), "application/json");
                 } catch (const std::exception &e) {
                     nlohmann::json err;
                     err["error"] = e.what();
                     res.status = 500;
                     res.set_content(err.dump(), "application/json");
                     std::cerr << "[API] Error: " << e.what() << "\n";
                 }
             });

    std::cout << "Listening on http://0.0.0.0:" << port << "\n";
    svr.listen("0.0.0.0", port);
    return 0;
}
