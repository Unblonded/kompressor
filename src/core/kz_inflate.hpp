// Deflate and Deflate64 ("enhanced deflate", ZIP method 9) decoding, written here (zlib has no
// Deflate64). Deflate64 is Deflate with a 64 KiB window, length code 285 meaning 3..65538 (16 extra
// bits) and distance codes 30 and 31.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace kz {

class Inflater {
public:
    // `src` fills a buffer with more compressed bytes (0 = no more).
    Inflater(std::function<size_t(uint8_t*, size_t)> src, bool deflate64);
    // Up to n bytes of output; 0 at the end of the stream or on an error (see error()).
    size_t read(uint8_t* out, size_t n);
    const std::string& error() const { return err_; }
    bool done() const { return done_ && pending_ == 0; }

private:
    bool bits(unsigned k, uint32_t& v);
    bool fill(unsigned k);
    bool header();
    bool buildTable(const uint8_t* lens, unsigned n, std::vector<uint32_t>& t);
    bool symbol(const std::vector<uint32_t>& t, unsigned& sym);
    bool step();  // decodes a little more into the window
    void put(uint8_t b);

    std::function<size_t(uint8_t*, size_t)> src_;
    bool d64_;
    std::vector<uint8_t> in_ = std::vector<uint8_t>(1 << 16);
    size_t inAt_ = 0, inHave_ = 0;
    bool inEnd_ = false;
    uint64_t bitbuf_ = 0;
    unsigned bitcnt_ = 0;

    static constexpr size_t kRing = 1 << 18;
    std::vector<uint8_t> ring_ = std::vector<uint8_t>(kRing);
    uint64_t pos_ = 0;      // bytes produced so far
    uint64_t pending_ = 0;  // produced but not yet handed out

    enum class St { Header, Stored, Huffman, End } st_ = St::Header;
    bool final_ = false, done_ = false;
    uint32_t storedLeft_ = 0;
    std::vector<uint32_t> lit_, dist_;
    std::string err_;
};

}  // namespace kz
