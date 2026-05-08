//
// Pipeline: owns all inter-thread queues and error propagation state
//
#ifndef FFMPEGPROJECT_PIPELINE_H
#define FFMPEGPROJECT_PIPELINE_H

#include <string>
#include <atomic>
#include <mutex>
#include "packet_queue.h"
#include "deep_copy_packey_queue.h"
#include "ring_buffer.h"

class Pipeline {
public:
    // --- Queues (moved here from global scope) ---
    PacketQueue<AVPacket> video_pkt_queue;
    PacketQueue<AVPacket> audio_pkt_queue;
    RingBuffer<AVFrame*> video_frame_ringbuf{30};
    RingBuffer<AVFrame*> audio_frame_ringbuf{30};
    DeepCopyPacketQueue en_video_pkt_queue;
    DeepCopyPacketQueue en_audio_pkt_queue;

    // --- Error propagation ---
    void report_error(const std::string& msg);
    bool has_error() const;
    std::string get_error() const;

private:
    std::atomic<bool> error_flag_{false};
    mutable std::mutex error_mtx_;
    std::string error_msg_;
};

#endif // FFMPEGPROJECT_PIPELINE_H
