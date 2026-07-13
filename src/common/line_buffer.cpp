#include "common/line_buffer.h"

#include <algorithm>

namespace lanspeak::common {

LineBuffer::LineBuffer(std::size_t maximum_pending_bytes)
    : maximum_pending_bytes_(std::max<std::size_t>(1, maximum_pending_bytes)) {
    storage_.reserve(std::min<std::size_t>(maximum_pending_bytes_, 4096));
}

void LineBuffer::append(std::string_view bytes) {
    if (bytes.empty()) {
        return;
    }
    compact();
    if (storage_.size() + bytes.size() > maximum_pending_bytes_) {
        clear();
        if (bytes.size() > maximum_pending_bytes_) {
            bytes.remove_prefix(bytes.size() - maximum_pending_bytes_);
        }
    }
    storage_.append(bytes);
}

bool LineBuffer::next(std::string& line) {
    const std::size_t newline = storage_.find('\n', consumed_);
    if (newline == std::string::npos) {
        return false;
    }
    line.assign(storage_, consumed_, newline - consumed_);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    consumed_ = newline + 1;
    if (consumed_ >= storage_.size()) {
        clear();
    } else if (consumed_ >= 4096 && consumed_ * 2 >= storage_.size()) {
        compact();
    }
    return true;
}

void LineBuffer::clear() {
    storage_.clear();
    consumed_ = 0;
}

std::size_t LineBuffer::pending_bytes() const {
    return storage_.size() - consumed_;
}

void LineBuffer::compact() {
    if (consumed_ == 0) {
        return;
    }
    storage_.erase(0, consumed_);
    consumed_ = 0;
}

} // namespace lanspeak::common
