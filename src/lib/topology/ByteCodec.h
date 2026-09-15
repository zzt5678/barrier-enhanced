#pragma once
#include <cstdint>
#include <string>

namespace weave { namespace topology { namespace detail {
inline void number(std::string& s, std::uint64_t n, unsigned bytes) {
    for (unsigned i = bytes; i > 0; --i) s += char(n >> ((i - 1) * 8));
}
inline void string(std::string& s, const std::string& v) {
    number(s, v.size(), 2); s += v;
}
struct Reader {
    const std::string& s;
    std::size_t p = 0;
    bool good = true;
    std::uint64_t number(unsigned bytes) {
        if (!good || bytes > s.size() - p) { good = false; return 0; }
        std::uint64_t n = 0;
        while (bytes--) n = (n << 8) | static_cast<unsigned char>(s[p++]);
        return n;
    }
    std::string string(std::size_t max) {
        const auto n = number(2);
        if (!good || n > max || n > s.size() - p) { good = false; return {}; }
        const auto result = s.substr(p, n); p += n; return result;
    }
    bool done() const { return good && p == s.size(); }
};
} } }
