#include "core/jitter_buffer.h"

#include <algorithm>
#include <cstring>

namespace lanspeak::core {

JitterBuffer::JitterBuffer(
    std::size_t start_threshold_frames,
    std::size_t maximum_buffer_frames)
    : start_threshold_frames_(start_threshold_frames),
      maximum_buffer_frames_(std::max<std::size_t>(1, maximum_buffer_frames)),
      ready_ring_(maximum_buffer_frames_) {}

void JitterBuffer::push(
    std::uint64_t sequence,
    const std::int16_t* samples,
    std::size_t frames) {
    push(sequence, std::span<const std::int16_t>(samples, samples == nullptr ? 0 : frames));
}

void JitterBuffer::push(
    std::uint64_t sequence,
    std::span<const std::int16_t> samples) {
    if (samples.empty()) {
        return;
    }

    std::lock_guard lock(mutex_);
    if (!have_sequence_) {
        expected_sequence_ = sequence;
        have_sequence_ = true;
    } else if (sequence < expected_sequence_) {
        if (expected_sequence_ - sequence > kSequenceRestartPacketGap) {
            reset_sequence_locked(sequence);
        } else {
            ++stats_.late_or_duplicate_packets;
            return;
        }
    } else if (sequence - expected_sequence_ > kSequenceRestartPacketGap) {
        reset_sequence_locked(sequence);
    }

    if (sequence == expected_sequence_) {
        append_ready_locked(samples);
        ++expected_sequence_;
        promote_locked();
    } else {
        std::vector<std::int16_t> packet(samples.begin(), samples.end());
        const auto [unused, inserted] = pending_packets_.emplace(sequence, std::move(packet));
        static_cast<void>(unused);
        if (!inserted) {
            ++stats_.late_or_duplicate_packets;
            return;
        }
    }

    ++stats_.packets_pushed;
    stats_.frames_pushed += samples.size();
    refresh_stats_locked();
}

bool JitterBuffer::push_pcm16_le(
    std::uint64_t sequence,
    std::span<const std::byte> payload) {
    if (payload.empty() || payload.size() % sizeof(std::int16_t) != 0) {
        return false;
    }

    const std::size_t frames = payload.size() / sizeof(std::int16_t);
    std::lock_guard lock(mutex_);
    if (!have_sequence_) {
        expected_sequence_ = sequence;
        have_sequence_ = true;
    } else if (sequence < expected_sequence_) {
        if (expected_sequence_ - sequence > kSequenceRestartPacketGap) {
            reset_sequence_locked(sequence);
        } else {
            ++stats_.late_or_duplicate_packets;
            return true;
        }
    } else if (sequence - expected_sequence_ > kSequenceRestartPacketGap) {
        reset_sequence_locked(sequence);
    }

    if (sequence == expected_sequence_) {
        append_ready_pcm16_le_locked(payload);
        ++expected_sequence_;
        promote_locked();
    } else {
        std::vector<std::int16_t> packet(frames);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto low = std::to_integer<std::uint8_t>(payload[frame * 2]);
            const auto high = std::to_integer<std::uint8_t>(payload[frame * 2 + 1]);
            packet[frame] = static_cast<std::int16_t>(
                static_cast<std::uint16_t>(low) |
                (static_cast<std::uint16_t>(high) << 8u));
        }
        const auto [unused, inserted] = pending_packets_.emplace(sequence, std::move(packet));
        static_cast<void>(unused);
        if (!inserted) {
            ++stats_.late_or_duplicate_packets;
            return true;
        }
    }

    ++stats_.packets_pushed;
    stats_.frames_pushed += frames;
    refresh_stats_locked();
    return true;
}

void JitterBuffer::pop_into(std::span<std::int16_t> output) {
    std::fill(output.begin(), output.end(), std::int16_t{0});
    if (output.empty()) {
        return;
    }

    std::lock_guard lock(mutex_);
    ++stats_.render_events;

    if (!stats_.started) {
        promote_locked();
        if (ready_size_ < start_threshold_frames_) {
            ++stats_.underrun_events;
            stats_.underrun_frames += output.size();
            ++stats_.startup_underrun_events;
            stats_.startup_underrun_frames += output.size();
            stats_.rendered_frames += output.size();
            refresh_stats_locked();
            return;
        }
        stats_.started = true;
    }

    std::size_t written = 0;
    bool underrun_recorded = false;
    while (written < output.size()) {
        if (ready_size_ == 0) {
            promote_locked();
            while (ready_size_ == 0 && have_sequence_ && !pending_packets_.empty()) {
                const auto first = pending_packets_.begin();
                if (first->first < expected_sequence_) {
                    pending_packets_.erase(first);
                    ++stats_.late_or_duplicate_packets;
                    continue;
                }
                stats_.sequence_gaps += first->first - expected_sequence_;
                expected_sequence_ = first->first;
                promote_locked();
            }
        }

        if (ready_size_ == 0) {
            const std::size_t missing = output.size() - written;
            if (!underrun_recorded) {
                ++stats_.underrun_events;
                ++stats_.after_start_underrun_events;
                underrun_recorded = true;
            }
            stats_.underrun_frames += missing;
            stats_.after_start_underrun_frames += missing;
            break;
        }

        written += copy_ready_locked(output.subspan(written));
    }

    stats_.rendered_frames += output.size();
    refresh_stats_locked();
}

void JitterBuffer::pop_into(std::vector<std::int16_t>& output, std::size_t frames) {
    output.resize(frames);
    pop_into(std::span<std::int16_t>(output));
}

void JitterBuffer::reset() {
    std::lock_guard lock(mutex_);
    pending_packets_.clear();
    read_index_ = 0;
    ready_size_ = 0;
    expected_sequence_ = 0;
    have_sequence_ = false;
    stats_.started = false;
    refresh_stats_locked();
}

JitterStats JitterBuffer::snapshot() const {
    std::lock_guard lock(mutex_);
    JitterStats copy = stats_;
    copy.current_frames = ready_size_;
    return copy;
}

void JitterBuffer::reset_sequence_locked(std::uint64_t sequence) {
    pending_packets_.clear();
    read_index_ = 0;
    ready_size_ = 0;
    expected_sequence_ = sequence;
    have_sequence_ = true;
    stats_.started = false;
    ++stats_.sequence_restarts;
    refresh_stats_locked();
}

void JitterBuffer::promote_locked() {
    while (have_sequence_) {
        const auto packet = pending_packets_.find(expected_sequence_);
        if (packet == pending_packets_.end()) {
            break;
        }
        append_ready_locked(packet->second);
        pending_packets_.erase(packet);
        ++expected_sequence_;
    }
}

void JitterBuffer::append_ready_locked(std::span<const std::int16_t> samples) {
    if (samples.empty()) {
        return;
    }

    const std::size_t overflow =
        ready_size_ + samples.size() > maximum_buffer_frames_
            ? ready_size_ + samples.size() - maximum_buffer_frames_
            : 0;
    if (overflow > 0) {
        const std::size_t old_frames_to_drop = std::min(overflow, ready_size_);
        discard_ready_locked(old_frames_to_drop);
        const std::size_t new_frames_to_skip = overflow - old_frames_to_drop;
        samples = samples.subspan(std::min(new_frames_to_skip, samples.size()));
        stats_.overflow_dropped_frames += overflow;
    }

    std::size_t source_offset = 0;
    while (source_offset < samples.size()) {
        const std::size_t write_index = (read_index_ + ready_size_) % maximum_buffer_frames_;
        const std::size_t chunk = std::min(
            samples.size() - source_offset,
            maximum_buffer_frames_ - write_index);
        std::memcpy(
            ready_ring_.data() + write_index,
            samples.data() + source_offset,
            chunk * sizeof(std::int16_t));
        ready_size_ += chunk;
        source_offset += chunk;
    }
    stats_.max_frames_seen = std::max(stats_.max_frames_seen, ready_size_);
}

void JitterBuffer::append_ready_pcm16_le_locked(std::span<const std::byte> payload) {
    const std::size_t frames = payload.size() / sizeof(std::int16_t);
    const std::size_t overflow =
        ready_size_ + frames > maximum_buffer_frames_
            ? ready_size_ + frames - maximum_buffer_frames_
            : 0;
    std::size_t source_frame = 0;
    if (overflow > 0) {
        const std::size_t old_frames_to_drop = std::min(overflow, ready_size_);
        discard_ready_locked(old_frames_to_drop);
        source_frame = overflow - old_frames_to_drop;
        stats_.overflow_dropped_frames += overflow;
    }

    for (; source_frame < frames; ++source_frame) {
        const auto low = std::to_integer<std::uint8_t>(payload[source_frame * 2]);
        const auto high = std::to_integer<std::uint8_t>(payload[source_frame * 2 + 1]);
        const std::size_t write_index = (read_index_ + ready_size_) % maximum_buffer_frames_;
        ready_ring_[write_index] = static_cast<std::int16_t>(
            static_cast<std::uint16_t>(low) |
            (static_cast<std::uint16_t>(high) << 8u));
        ++ready_size_;
    }
    stats_.max_frames_seen = std::max(stats_.max_frames_seen, ready_size_);
}

void JitterBuffer::discard_ready_locked(std::size_t frames) {
    const std::size_t discarded = std::min(frames, ready_size_);
    read_index_ = (read_index_ + discarded) % maximum_buffer_frames_;
    ready_size_ -= discarded;
}

std::size_t JitterBuffer::copy_ready_locked(std::span<std::int16_t> output) {
    const std::size_t frames = std::min(output.size(), ready_size_);
    std::size_t copied = 0;
    while (copied < frames) {
        const std::size_t chunk = std::min(frames - copied, maximum_buffer_frames_ - read_index_);
        std::memcpy(
            output.data() + copied,
            ready_ring_.data() + read_index_,
            chunk * sizeof(std::int16_t));
        read_index_ = (read_index_ + chunk) % maximum_buffer_frames_;
        ready_size_ -= chunk;
        copied += chunk;
    }
    return copied;
}

void JitterBuffer::refresh_stats_locked() {
    stats_.current_frames = ready_size_;
}

} // namespace lanspeak::core
