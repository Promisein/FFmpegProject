//
// Unit tests for 3 queue implementations
//
#include <gtest/gtest.h>
#include "packet_queue.h"
#include "ring_buffer.h"
#include "deep_copy_packey_queue.h"
#include "ffmpeg_raii.h"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

// ====================== PacketQueue Tests ======================

TEST(PacketQueueTest, FIFOOrder) {
    PacketQueue<int> q;
    for (int i = 0; i < 100; i++) {
        q.push(i);
    }
    for (int i = 0; i < 100; i++) {
        int val;
        ASSERT_TRUE(q.pop(val));
        EXPECT_EQ(val, i) << "FIFO order violated at index " << i;
    }
}

TEST(PacketQueueTest, BlockingPop) {
    PacketQueue<int> q;
    std::atomic<bool> push_done{false};

    std::thread producer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        q.push(42);
        push_done = true;
    });

    int val;
    ASSERT_TRUE(q.pop(val));
    EXPECT_EQ(val, 42);

    producer.join();
    EXPECT_TRUE(push_done);
}

TEST(PacketQueueTest, MultiProducerConsumer) {
    PacketQueue<int> q;
    const int items_per_producer = 5000;
    std::atomic<int64_t> consumed_sum{0};
    std::atomic<int> consumed_count{0};

    // 2 producers
    std::thread p1([&]() {
        for (int i = 0; i < items_per_producer; i++) q.push(i);
    });
    std::thread p2([&]() {
        for (int i = items_per_producer; i < items_per_producer * 2; i++) q.push(i);
    });

    // 2 consumers
    std::thread c1([&]() {
        int val;
        for (int i = 0; i < items_per_producer; i++) {
            if (q.pop(val)) {
                consumed_sum += val;
                consumed_count++;
            }
        }
    });
    std::thread c2([&]() {
        int val;
        for (int i = 0; i < items_per_producer; i++) {
            if (q.pop(val)) {
                consumed_sum += val;
                consumed_count++;
            }
        }
    });

    p1.join(); p2.join();
    c1.join(); c2.join();

    EXPECT_EQ(consumed_count.load(), items_per_producer * 2);
    // Sum of 0..9999 = 49995000
    EXPECT_EQ(consumed_sum.load(), 49995000);
}

// ====================== RingBuffer Tests ======================

class RingBufferTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Allocate 10 test frames with valid buffers (required for av_frame_ref)
        for (int i = 0; i < 10; i++) {
            test_frames[i] = av_frame_alloc();
            ASSERT_NE(test_frames[i], nullptr);
            test_frames[i]->format = AV_PIX_FMT_YUV420P;
            test_frames[i]->width = 4;
            test_frames[i]->height = 4;
            ASSERT_GE(av_frame_get_buffer(test_frames[i], 32), 0);
        }
    }

    void TearDown() override {
        for (int i = 0; i < 10; i++) {
            if (test_frames[i]) {
                av_frame_free(&test_frames[i]);
            }
        }
    }

    AVFrame* test_frames[10] = {};
};

TEST_F(RingBufferTest, FIFOOrder) {
    RingBuffer<AVFrame*> rb(5);
    // Push 5 frames with labeled PTS
    for (int i = 0; i < 5; i++) {
        test_frames[i]->pts = i * 10;
        ASSERT_TRUE(rb.push(test_frames[i]));
    }

    // Pop 5 frames, verify PTS order
    for (int i = 0; i < 5; i++) {
        AVFrame* out = av_frame_alloc();
        ASSERT_TRUE(rb.pop(out));
        EXPECT_EQ(out->pts, i * 10);
        av_frame_free(&out);
    }
}

TEST_F(RingBufferTest, FullBlocksProducer) {
    RingBuffer<AVFrame*> rb(3);

    // Fill buffer
    for (int i = 0; i < 3; i++) {
        ASSERT_TRUE(rb.push(test_frames[i]));
    }

    // 4th push should block - verify via async with timeout
    std::atomic<bool> push_returned{false};
    std::thread producer([&]() {
        bool ret = rb.push(test_frames[3]);
        push_returned = ret;
    });

    // Wait 200ms - push should still be blocked
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_FALSE(push_returned) << "4th push should block when buffer is full";

    // Pop one to unblock
    AVFrame* out = av_frame_alloc();
    ASSERT_TRUE(rb.pop(out));
    av_frame_free(&out);

    // Give producer time to complete
    producer.join();
    EXPECT_TRUE(push_returned);
}

TEST_F(RingBufferTest, FlushUnblocksProducer) {
    RingBuffer<AVFrame*> rb(2);

    // Fill buffer
    ASSERT_TRUE(rb.push(test_frames[0]));
    ASSERT_TRUE(rb.push(test_frames[1]));

    // 3rd push blocks
    std::atomic<bool> push_returned{false};
    std::thread producer([&]() {
        push_returned = rb.push(test_frames[2]);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Flush should unblock with false
    rb.flush();

    producer.join();
    EXPECT_FALSE(push_returned) << "push should return false after flush";
}

// ====================== DeepCopyPacketQueue Tests ======================

TEST(DeepCopyPacketQueueTest, PushPopDataIntegrity) {
    DeepCopyPacketQueue q;

    // Create a packet with known data
    PacketPtr pkt_in(av_packet_alloc());
    ASSERT_NE(pkt_in, nullptr);
    uint8_t test_data[10] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    pkt_in->data = test_data;
    pkt_in->size = sizeof(test_data);
    pkt_in->pts = 12345;

    q.push(*pkt_in);

    PacketPtr pkt_out(av_packet_alloc());
    ASSERT_NE(pkt_out, nullptr);
    ASSERT_TRUE(q.pop(*pkt_out));
    EXPECT_EQ(pkt_out->size, 10);
    EXPECT_EQ(pkt_out->pts, 12345);
    EXPECT_EQ(memcmp(pkt_out->data, test_data, 10), 0) << "Data should be preserved";
}

TEST(DeepCopyPacketQueueTest, DeepCopyVerification) {
    DeepCopyPacketQueue q;

    PacketPtr pkt_in(av_packet_alloc());
    ASSERT_NE(pkt_in, nullptr);
    uint8_t original_data[5] = {1, 2, 3, 4, 5};
    pkt_in->data = original_data;
    pkt_in->size = 5;

    q.push(*pkt_in);

    // Modify the original data
    original_data[0] = 99;
    original_data[2] = 99;

    PacketPtr pkt_out(av_packet_alloc());
    ASSERT_NE(pkt_out, nullptr);
    ASSERT_TRUE(q.pop(*pkt_out));
    EXPECT_EQ(pkt_out->data[0], 1) << "Should be deep-copied, not affected by source mod";
    EXPECT_EQ(pkt_out->data[2], 3) << "Should be deep-copied, not affected by source mod";
}

TEST(DeepCopyPacketQueueTest, MarkDoneReturnsFalseOnPop) {
    DeepCopyPacketQueue q;
    q.mark_done();

    PacketPtr pkt(av_packet_alloc());
    ASSERT_NE(pkt, nullptr);
    EXPECT_FALSE(q.pop(*pkt)) << "pop should return false after mark_done on empty queue";
}
