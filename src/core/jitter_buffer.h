#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <vector>

namespace lanspeak::core {

inline constexpr std::uint64_t kSequenceRestartPacketGap = 256;

struct JitterStats {
    std::uint64_t packets_pushed = 0;
    std::uint64_t frames_pushed = 0;
    std::uint64_t late_or_duplicate_packets = 0;
    std::uint64_t sequence_gaps = 0;
    std::uint64_t sequence_restarts = 0;
    std::uint64_t overflow_dropped_frames = 0;
    std::uint64_t underrun_events = 0;
    std::uint64_t underrun_frames = 0;
    std::uint64_t startup_underrun_events = 0;
    std::uint64_t startup_underrun_frames = 0;
    std::uint64_t after_start_underrun_events = 0;
    std::uint64_t after_start_underrun_frames = 0;
    std::uint64_t rendered_frames = 0;
    std::uint64_t render_events = 0;
    std::size_t current_frames = 0;
    std::size_t max_frames_seen = 0;
    bool started = false;
};

class JitterBuffer {
public:
    JitterBuffer(std::size_t start_threshold_frames, std::size_t maximum_buffer_frames);

    void push(std::uint64_t sequence, std::span<const std::int16_t> samples);
    void push(std::uint64_t sequence, const std::int16_t* samples, std::size_t frames);
    bool push_pcm16_le(std::uint64_t sequence, std::span<const std::byte> payload);
    void pop_into(std::span<std::int16_t> output);
    void pop_into(std::vector<std::int16_t>& output, std::size_t frames);
    void reset();
    [[nodiscard]] JitterStats snapshot() const;

private:
    void reset_sequence_locked(std::uint64_t sequence);
    void promote_locked();
    void append_ready_locked(std::span<const std::int16_t> samples);
    void append_ready_pcm16_le_locked(std::span<const std::byte> payload);
    void discard_ready_locked(std::size_t frames);
    std::size_t copy_ready_locked(std::span<std::int16_t> output);
    void refresh_stats_locked();

    std::size_t start_threshold_frames_ = 0;
    std::size_t maximum_buffer_frames_ = 0;
    mutable std::mutex mutex_;
    std::map<std::uint64_t, std::vector<std::int16_t>> pending_packets_;
    std::vector<std::int16_t> ready_ring_;
    std::size_t read_index_ = 0;
    std::size_t ready_size_ = 0;
    std::uint64_t expected_sequence_ = 0;
    bool have_sequence_ = false;
    JitterStats stats_;
};

} // namespace lanspeak::core
