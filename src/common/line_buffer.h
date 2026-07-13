#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace lanspeak::common {

class LineBuffer {
public:
    explicit LineBuffer(std::size_t maximum_pending_bytes = 64 * 1024);

    void append(std::string_view bytes);
    bool next(std::string& line);
    void clear();
    [[nodiscard]] std::size_t pending_bytes() const;

private:
    void compact();

    std::size_t maximum_pending_bytes_ = 0;
    std::string storage_;
    std::size_t consumed_ = 0;
};

} // namespace lanspeak::common
