cmake_minimum_required(VERSION 3.27)
project(FFmpegProject)

set(CMAKE_CXX_STANDARD 17)

# 定义 FFmpeg 路径
set(FFMPEG_ROOT "C:/Users/Jianing/Desktop/FFmpegProject/ffmpeg-4.4-full_build-shared")
set(SRC_ROOT ${CMAKE_SOURCE_DIR}/src)

add_executable(FFmpegProject
        main.cpp
        ${SRC_ROOT}/common.cpp
        ${SRC_ROOT}/ring_buffer.cpp
        ${SRC_ROOT}/demux.cpp
        ${SRC_ROOT}/videodecoder.cpp
        ${SRC_ROOT}/audiodecoder.cpp
        ${SRC_ROOT}/videoencoder.cpp
        ${SRC_ROOT}/audioencoder.cpp
        ${SRC_ROOT}/mux.cpp

)


# 设置库文件搜索路径
link_directories(${CMAKE_SOURCE_DIR}/lib)

target_include_directories(FFmpegProject PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(FFmpegProject PRIVATE
        ${FFMPEG_ROOT}/lib/libavcodec.dll.a
        ${FFMPEG_ROOT}/lib/libavdevice.dll.a
        ${FFMPEG_ROOT}/lib/libavfilter.dll.a
        ${FFMPEG_ROOT}/lib/libavformat.dll.a
        ${FFMPEG_ROOT}/lib/libavutil.dll.a
        ${FFMPEG_ROOT}/lib/libpostproc.dll.a
        ${FFMPEG_ROOT}/lib/libswresample.dll.a
        ${FFMPEG_ROOT}/lib/libswscale.dll.a
)