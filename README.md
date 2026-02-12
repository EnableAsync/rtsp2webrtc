# rtsp2webrtc

C++ RTSP to WebRTC 转发服务。内置 HTTP 信令 + Web 播放器，支持 H.265→H.264 转码。

## 架构

```
浏览器 ──POST /api/offer──→ HTTP Server
                              │
                    创建 PeerConnection + H.264 Track
                    设置 offer, 生成 answer
                              │
浏览器 ←── answer JSON ──────┘
                              │
            RTSP Reader (FFmpeg) → [转码如需] → libdatachannel Track → 浏览器
```

## 依赖

系统需安装：

- CMake >= 3.20
- C++17 编译器
- OpenSSL
- x264
- pkg-config

以下依赖由 CMake 自动拉取：

| 库 | 方式 | 用途 |
|---|---|---|
| FFmpeg 7.1.1 | ExternalProject | RTSP 拉流 / H.265 解码 / H.264 编码 |
| libdatachannel | FetchContent | WebRTC |
| cpp-httplib | FetchContent | HTTP 服务 |
| nlohmann_json | FetchContent | JSON 解析 |

## 构建

```bash
cmake -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j$(nproc)
```

首次构建需下载并编译 FFmpeg，耗时较长。

## 运行

```bash
./build/rtsp2webrtc          # 默认端口 8080
./build/rtsp2webrtc 9090     # 自定义端口
```

浏览器打开 `http://localhost:8080`，输入 RTSP URL，点 Play。

## API

```
POST /api/offer
Content-Type: application/json

{
  "rtsp_url": "rtsp://user:pass@host:554/stream",
  "sdp": "v=0\r\n..."
}

Response:
{
  "type": "answer",
  "sdp": "v=0\r\n..."
}
```

## 文件结构

```
src/
├── main.cpp             # HTTP 服务 + 信令
├── rtsp_reader.h/cpp    # FFmpeg RTSP 拉流 + Annex-B NAL 解析
├── transcoder.h/cpp     # H.265→H.264 转码 (含 swscale)
├── webrtc_session.h/cpp # libdatachannel PeerConnection
└── stream_manager.h/cpp # RTSP 源管理 + 多观众分发
web/
└── index.html           # Web 播放器 (同时内嵌于 main.cpp)
```

## 特性

- HTTP-only 信令，无需 WebSocket
- 多路 RTSP 源，URL 在请求中指定
- 多观众共享同一 RTSP 连接
- H.264 直通，H.265 自动转码为 H.264

## 测试方法
1. 启动 rtsp server
```bash
./mediamtx
```

2. 推送视频
```bash
ffmpeg -re -stream_loop -1 -i test.mp4 \
    -c:v libx264 \
    -b:v 1000k -maxrate 1000k -bufsize 2000k \
    -g 30 \
    -preset ultrafast -tune zerolatency \
    -c:a copy \
    -f rtsp \
    rtsp://localhost:8554/mystream
```

3. 测试
```bash
./build/rtsp2webrtc 8080
```