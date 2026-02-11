cmake_minimum_required(VERSION 3.20)
project(rtsp2webrtc VERSION 0.1.0 LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(ExternalProject)
include(FetchContent)

include(ProcessorCount)
ProcessorCount(NPROC)
if(NPROC EQUAL 0)
    set(NPROC 4)
endif()

# ==== FFmpeg (ExternalProject) ====
set(FFMPEG_INSTALL_DIR ${CMAKE_BINARY_DIR}/ffmpeg-install)

# Find system x264 pkg-config path
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(X264 QUIET x264)
endif()

set(FFMPEG_EXTRA_CFLAGS "")
set(FFMPEG_EXTRA_LDFLAGS "")
if(X264_INCLUDE_DIRS)
    set(FFMPEG_EXTRA_CFLAGS "-I${X264_INCLUDE_DIRS}")
endif()
if(X264_LIBRARY_DIRS)
    set(FFMPEG_EXTRA_LDFLAGS "-L${X264_LIBRARY_DIRS}")
endif()

ExternalProject_Add(ffmpeg_ext
    URL https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz
    PREFIX ${CMAKE_BINARY_DIR}/ffmpeg-prefix
    CONFIGURE_COMMAND <SOURCE_DIR>/configure
        --prefix=${FFMPEG_INSTALL_DIR}
        --disable-everything
        --disable-programs
        --disable-doc
        --disable-avdevice
        --disable-postproc
        --disable-avfilter
        --disable-swresample
        --disable-x86asm
        --disable-autodetect
        --enable-network
        --enable-protocol=file,tcp,udp,rtp,http
        --enable-demuxer=rtsp,rtp,sdp
        --enable-muxer=null
        --enable-decoder=h264,hevc
        --enable-encoder=libx264
        --enable-parser=h264,hevc
        --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,extract_extradata
        --enable-gpl
        --enable-libx264
        --enable-swscale
        --enable-pic
        --enable-static
        --disable-shared
        --extra-cflags=${FFMPEG_EXTRA_CFLAGS}
        --extra-ldflags=${FFMPEG_EXTRA_LDFLAGS}
    BUILD_COMMAND make -j${NPROC}
    INSTALL_COMMAND make install
    BUILD_IN_SOURCE 1
)

# Pre-create FFmpeg install include dir (needed for IMPORTED target validation)
file(MAKE_DIRECTORY ${FFMPEG_INSTALL_DIR}/include)

# Create imported targets for FFmpeg libs
foreach(_lib avformat avcodec swscale avutil)
    add_library(ff${_lib} STATIC IMPORTED)
    set_target_properties(ff${_lib} PROPERTIES
        IMPORTED_LOCATION ${FFMPEG_INSTALL_DIR}/lib/lib${_lib}.a
        INTERFACE_INCLUDE_DIRECTORIES ${FFMPEG_INSTALL_DIR}/include
    )
    add_dependencies(ff${_lib} ffmpeg_ext)
endforeach()

# ==== libdatachannel (FetchContent) ====
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)

FetchContent_Declare(libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.22.5
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(libdatachannel)

# ==== cpp-httplib (FetchContent, header-only) ====
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.3
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(httplib)

# ==== nlohmann_json (FetchContent, header-only) ====
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(json)

# ==== Main target ====
add_executable(rtsp2webrtc
    src/main.cpp
    src/rtsp_reader.cpp
    src/transcoder.cpp
    src/webrtc_session.cpp
    src/stream_manager.cpp
)

add_dependencies(rtsp2webrtc ffmpeg_ext)

target_include_directories(rtsp2webrtc PRIVATE
    ${FFMPEG_INSTALL_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(rtsp2webrtc PRIVATE
    ffavformat ffavcodec ffswscale ffavutil
    LibDataChannel::LibDataChannel
    httplib::httplib
    nlohmann_json::nlohmann_json
    x264
    pthread
    z
    m
)
