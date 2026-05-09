//
// Unit tests for VideoRotateProcessor
//
#include <gtest/gtest.h>
#include "video_rotate.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

// ====================== Test Helpers ======================

// Helper: create a 4x4 YUV420P frame with known pixel values
// Y plane (4x4): linear index y*width+x (0..15)
// U plane (2x2): 100, 101, 102, 103
// V plane (2x2): 200, 201, 202, 203
static AVFrame* create_test_yuv420p_frame() {
    AVFrame* frame = av_frame_alloc();
    if (!frame) return nullptr;

    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = 4;
    frame->height = 4;

    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }

    // Fill Y plane with linear pattern 0..15
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            frame->data[0][y * frame->linesize[0] + x] = static_cast<uint8_t>(y * 4 + x);
        }
    }

    // Fill U plane with 100..103
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            frame->data[1][y * frame->linesize[1] + x] = static_cast<uint8_t>(100 + y * 2 + x);
        }
    }

    // Fill V plane with 200..203
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            frame->data[2][y * frame->linesize[2] + x] = static_cast<uint8_t>(200 + y * 2 + x);
        }
    }

    return frame;
}

// ====================== Test Fixture ======================

class RotationTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_frame = create_test_yuv420p_frame();
        ASSERT_NE(test_frame, nullptr) << "Failed to create test YUV420P frame";
    }

    void TearDown() override {
        if (test_frame) {
            av_frame_free(&test_frame);
        }
    }

    AVFrame* test_frame = nullptr;
};

// ====================== Rotation Test Cases ======================

TEST_F(RotationTest, Rotate90CW) {
    AVFrame* rotated = VideoRotateProcessor::rotateFrameDirect(test_frame, ROTATE_90_CW);
    ASSERT_NE(rotated, nullptr);
    EXPECT_EQ(rotated->width, test_frame->height);
    EXPECT_EQ(rotated->height, test_frame->width);

    // Verify Y plane: src(y,x) -> dst(x, height-y-1)
    // src(0,0)=0 -> dst(0,3)=0; src(0,1)=1 -> dst(1,3)=1; src(0,2)=2 -> dst(2,3)=2
    EXPECT_EQ(rotated->data[0][0 * rotated->linesize[0] + 3], 0);
    EXPECT_EQ(rotated->data[0][1 * rotated->linesize[0] + 3], 1);
    EXPECT_EQ(rotated->data[0][2 * rotated->linesize[0] + 3], 2);
    EXPECT_EQ(rotated->data[0][3 * rotated->linesize[0] + 3], 3);

    // src(3,0)=12 -> dst(0,0)=12
    EXPECT_EQ(rotated->data[0][0 * rotated->linesize[0] + 0], 12);
    // src(3,3)=15 -> dst(3,0)=15
    EXPECT_EQ(rotated->data[0][3 * rotated->linesize[0] + 0], 15);

    // Verify U plane (2x2): src(y,x) -> dst(x, 1-y)  [height/2 - y - 1]
    EXPECT_EQ(rotated->data[1][0 * rotated->linesize[1] + 1], 100);  // src(0,0) -> dst(0,1)
    EXPECT_EQ(rotated->data[1][1 * rotated->linesize[1] + 1], 101);  // src(0,1) -> dst(1,1)
    EXPECT_EQ(rotated->data[1][0 * rotated->linesize[1] + 0], 102);  // src(1,0) -> dst(0,0)
    EXPECT_EQ(rotated->data[1][1 * rotated->linesize[1] + 0], 103);  // src(1,1) -> dst(1,0)

    // Verify V plane (2x2)
    EXPECT_EQ(rotated->data[2][0 * rotated->linesize[2] + 1], 200);
    EXPECT_EQ(rotated->data[2][1 * rotated->linesize[2] + 1], 201);
    EXPECT_EQ(rotated->data[2][0 * rotated->linesize[2] + 0], 202);
    EXPECT_EQ(rotated->data[2][1 * rotated->linesize[2] + 0], 203);

    av_frame_free(&rotated);
}

TEST_F(RotationTest, Rotate180) {
    AVFrame* rotated = VideoRotateProcessor::rotateFrameDirect(test_frame, ROTATE_180);
    ASSERT_NE(rotated, nullptr);
    EXPECT_EQ(rotated->width, test_frame->width);
    EXPECT_EQ(rotated->height, test_frame->height);

    // Verify Y plane: src(y,x) -> dst(height-y-1, width-x-1)
    // src(0,0)=0 -> dst(3,3)=0
    EXPECT_EQ(rotated->data[0][3 * rotated->linesize[0] + 3], 0);
    // src(0,3)=3 -> dst(3,0)=3
    EXPECT_EQ(rotated->data[0][3 * rotated->linesize[0] + 0], 3);
    // src(3,0)=12 -> dst(0,3)=12
    EXPECT_EQ(rotated->data[0][0 * rotated->linesize[0] + 3], 12);
    // src(3,3)=15 -> dst(0,0)=15
    EXPECT_EQ(rotated->data[0][0 * rotated->linesize[0] + 0], 15);

    // Verify U plane: src(0,0)=100 -> dst(1,1)=100
    EXPECT_EQ(rotated->data[1][1 * rotated->linesize[1] + 1], 100);
    // src(0,1)=101 -> dst(1,0)=101
    EXPECT_EQ(rotated->data[1][1 * rotated->linesize[1] + 0], 101);
    // src(1,0)=102 -> dst(0,1)=102
    EXPECT_EQ(rotated->data[1][0 * rotated->linesize[1] + 1], 102);
    // src(1,1)=103 -> dst(0,0)=103
    EXPECT_EQ(rotated->data[1][0 * rotated->linesize[1] + 0], 103);

    // Verify V plane
    EXPECT_EQ(rotated->data[2][1 * rotated->linesize[2] + 1], 200);
    EXPECT_EQ(rotated->data[2][1 * rotated->linesize[2] + 0], 201);
    EXPECT_EQ(rotated->data[2][0 * rotated->linesize[2] + 1], 202);
    EXPECT_EQ(rotated->data[2][0 * rotated->linesize[2] + 0], 203);

    av_frame_free(&rotated);
}

TEST_F(RotationTest, Rotate270CW) {
    AVFrame* rotated = VideoRotateProcessor::rotateFrameDirect(test_frame, ROTATE_270_CW);
    ASSERT_NE(rotated, nullptr);
    EXPECT_EQ(rotated->width, test_frame->height);
    EXPECT_EQ(rotated->height, test_frame->width);

    // Verify Y plane: src(y,x) -> dst(width-x-1, y)
    // src(0,0)=0 -> dst(3,0)=0
    EXPECT_EQ(rotated->data[0][3 * rotated->linesize[0] + 0], 0);
    // src(0,3)=3 -> dst(0,0)=3
    EXPECT_EQ(rotated->data[0][0 * rotated->linesize[0] + 0], 3);
    // src(3,0)=12 -> dst(3,3)=12
    EXPECT_EQ(rotated->data[0][3 * rotated->linesize[0] + 3], 12);
    // src(3,3)=15 -> dst(0,3)=15
    EXPECT_EQ(rotated->data[0][0 * rotated->linesize[0] + 3], 15);

    // Verify U plane: src(0,0)=100 -> dst(1,0)=100
    EXPECT_EQ(rotated->data[1][1 * rotated->linesize[1] + 0], 100);
    // src(0,1)=101 -> dst(0,0)=101
    EXPECT_EQ(rotated->data[1][0 * rotated->linesize[1] + 0], 101);
    // src(1,0)=102 -> dst(1,1)=102
    EXPECT_EQ(rotated->data[1][1 * rotated->linesize[1] + 1], 102);
    // src(1,1)=103 -> dst(0,1)=103
    EXPECT_EQ(rotated->data[1][0 * rotated->linesize[1] + 1], 103);

    // Verify V plane
    EXPECT_EQ(rotated->data[2][1 * rotated->linesize[2] + 0], 200);
    EXPECT_EQ(rotated->data[2][0 * rotated->linesize[2] + 0], 201);
    EXPECT_EQ(rotated->data[2][1 * rotated->linesize[2] + 1], 202);
    EXPECT_EQ(rotated->data[2][0 * rotated->linesize[2] + 1], 203);

    av_frame_free(&rotated);
}

TEST_F(RotationTest, RotateDimensions90) {
    VideoRotateProcessor proc(ROTATE_90_CW);
    int dst_w = 0, dst_h = 0;
    proc.getRotatedDimensions(1920, 1080, &dst_w, &dst_h);
    EXPECT_EQ(dst_w, 1080);
    EXPECT_EQ(dst_h, 1920);
}

TEST_F(RotationTest, RotateDimensions180) {
    VideoRotateProcessor proc(ROTATE_180);
    int dst_w = 0, dst_h = 0;
    proc.getRotatedDimensions(1920, 1080, &dst_w, &dst_h);
    EXPECT_EQ(dst_w, 1920);
    EXPECT_EQ(dst_h, 1080);
}

TEST_F(RotationTest, UnsupportedFormat) {
    // Create a frame with unsupported pixel format (e.g., GRAY8)
    AVFrame* gray_frame = av_frame_alloc();
    ASSERT_NE(gray_frame, nullptr);
    gray_frame->format = AV_PIX_FMT_GRAY8;
    gray_frame->width = 4;
    gray_frame->height = 4;
    ASSERT_GE(av_frame_get_buffer(gray_frame, 32), 0);

    AVFrame* result = VideoRotateProcessor::rotateFrameDirect(gray_frame, ROTATE_90_CW);
    EXPECT_EQ(result, nullptr) << "Unsupported format should return nullptr";

    av_frame_free(&gray_frame);
}

TEST_F(RotationTest, IsFormatSupported) {
    EXPECT_TRUE(VideoRotateProcessor::isFormatSupported(AV_PIX_FMT_YUV420P));
    EXPECT_FALSE(VideoRotateProcessor::isFormatSupported(AV_PIX_FMT_GRAY8));
    EXPECT_FALSE(VideoRotateProcessor::isFormatSupported(AV_PIX_FMT_RGB24));
    EXPECT_FALSE(VideoRotateProcessor::isFormatSupported(AV_PIX_FMT_YUV444P));
}
