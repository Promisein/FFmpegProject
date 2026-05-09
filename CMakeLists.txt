cmake_minimum_required(VERSION 3.27)
project(FFmpegProject)

set(CMAKE_CXX_STANDARD 17)

# 定义 FFmpeg 路径
set(FFMPEG_ROOT "C:/Users/Jianing/Desktop/C++Project/FFmpegProject/ffmpeg-4.4-full_build-shared")
set(SRC_ROOT ${CMAKE_SOURCE_DIR}/src)

# ====================== 主程序 ======================
add_executable(FFmpegProject
        main.cpp
        ${SRC_ROOT}/thread_pool.cpp
        ${SRC_ROOT}/pipeline.cpp
        ${SRC_ROOT}/common.cpp
        ${SRC_ROOT}/ring_buffer.cpp
        ${SRC_ROOT}/demux.cpp
        ${SRC_ROOT}/videodecoder.cpp
        ${SRC_ROOT}/audiodecoder.cpp
        ${SRC_ROOT}/videoencoder.cpp
        ${SRC_ROOT}/audioencoder.cpp
        ${SRC_ROOT}/mux.cpp
        ${SRC_ROOT}/video_rotate.cpp
        ${SRC_ROOT}/logger.cpp

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

# ====================== 单元测试 ======================
include(FetchContent)
FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(FFmpegProject_tests
        tests/test_queues.cpp
        tests/test_rotation.cpp
        ${SRC_ROOT}/video_rotate.cpp
)

target_include_directories(FFmpegProject_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(FFmpegProject_tests PRIVATE
        gtest_main
        ${FFMPEG_ROOT}/lib/libavcodec.dll.a
        ${FFMPEG_ROOT}/lib/libavformat.dll.a
        ${FFMPEG_ROOT}/lib/libavutil.dll.a
        ${FFMPEG_ROOT}/lib/libswscale.dll.a
)

add_test(NAME FFmpegProject_tests COMMAND FFmpegProject_tests)
