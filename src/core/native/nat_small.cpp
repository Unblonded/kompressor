// Small single-purpose containers 7-Zip opens:
//   MsLZ   - MS-DOS/Windows "COMPRESS.EXE" files (SZDD, setup.ex_ and friends): LZSS in a 4 KiB window.
//   SWFc   - compressed Flash (CWS = zlib, ZWS = LZMA): the plain .swf inside.
//   SWF    - plain Flash: its embedded JPEG pictures and MP3 sounds.
//   FLV    - Flash video: the audio and video streams as playable files (.mp3, .aac, .264...).
//   Base64 - base64 text (.b64), decoded.
//   IHex   - Intel HEX firmware images: one binary per contiguous address range.
#include <zlib.h>

#include <algorithm>
#include <map>

#include "nat.hpp"

namespace kz::native {

namespace {

std::string stem(const std::string& name) {
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}
std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string extOf(const std::string& name) {
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? "" : lower(name.substr(dot + 1));
}

// ============================================================================================== MsLZ
bool openMslz(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> h = s->getUpTo(0, 14);
    Span H(h);
    if (!H.eq(0, "SZDD\x88\xF0\x27\x33", 8) || h.size() < 14) return false;
    char missing = (char)H.u8(9);
    uint32_t outLen = H.u32(10);
    if (s->size() > (256u << 20) || outLen > (1u << 30)) return false;
    std::vector<uint8_t> in = s->get(14, (size_t)(s->size() - 14));
    std::vector<uint8_t> out;
    out.reserve(outLen);
    uint8_t win[4096];
    std::memset(win, ' ', sizeof win);
    uint32_t wp = 4096 - 16;
    size_t i = 0;
    while (i < in.size() && out.size() < outLen) {
        uint8_t flags = in[i++];
        for (int bit = 0; bit < 8 && i < in.size() && out.size() < outLen; ++bit) {
            if (flags & (1 << bit)) {
                uint8_t c = in[i++];
                out.push_back(c);
                win[wp] = c;
                wp = (wp + 1) & 4095;
            } else {
                if (i + 1 >= in.size()) break;
                uint32_t pos = in[i] | (in[i + 1] & 0xF0) << 4, len = (in[i + 1] & 0x0F) + 3u;
                i += 2;
                for (uint32_t k = 0; k < len && out.size() < outLen; ++k) {
                    uint8_t c = win[(pos + k) & 4095];
                    out.push_back(c);
                    win[wp] = c;
                    wp = (wp + 1) & 4095;
                }
            }
        }
    }
    if (out.size() < outLen) ctx.out.warning = "The compressed file ends early";
    // The name loses its last letter to "_" ("setup.ex_"); the header keeps the letter.
    std::string name = ctx.name.empty() ? "file" : ctx.name;
    if (!name.empty() && name.back() == '_') {
        if (missing) name.back() = missing;
        else name.pop_back();
    } else if (name == ctx.name) {
        name = stem(name);
    }
    ctx.formats.push_back("MsLZ");
    ctx.out.items.push_back(builtItem(ctx.prefix + component(name), std::move(out), 0));
    return true;
}

// =============================================================================================== SWF
// Compressed Flash: the uncompressed movie, rebuilt with an "FWS" header.
bool openSwfc(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> h = s->getUpTo(0, 17);
    Span H(h);
    if (h.size() < 8 || (!H.eq(0, "CWS", 3) && !H.eq(0, "ZWS", 3))) return false;
    uint32_t len = H.u32(4);
    if (len < 8 || len > (1u << 30) || s->size() > (1u << 30)) return false;
    std::vector<uint8_t> body;
    bool ok;
    if (h[0] == 'C') {
        std::vector<uint8_t> in = s->get(8, (size_t)(s->size() - 8));
        ok = inflateZlib(in.data(), in.size(), body, len - 8);
    } else {
        if (h.size() < 17) return false;
        std::vector<uint8_t> in = s->get(17, (size_t)(s->size() - 17));
        ok = unLzmaRaw(h.data() + 12, in.data(), in.size(), body, len - 8);
    }
    if (!ok && body.empty()) {
        ctx.err = "Damaged compressed Flash file";
        return false;
    }
    std::vector<uint8_t> out{'F', 'W', 'S', h[3]};
    put32(out, len);
    out.insert(out.end(), body.begin(), body.end());
    if (body.size() < len - 8) ctx.out.warning = "The compressed movie ends early";
    ctx.formats.push_back("SWFc");
    // Named like 7-Zip names it: "movie.swf" holds "movie~.swf".
    std::string name = (ctx.name.empty() ? "movie" : stem(ctx.name)) + "~.swf";
    ctx.out.items.push_back(builtItem(ctx.prefix + component(name), std::move(out), 0));
    return true;
}

// Plain Flash: pictures and sounds from the tag list.
bool openSwf(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> h = s->getUpTo(0, 32);
    Span H(h);
    if (h.size() < 21 || !H.eq(0, "FWS", 3) || h[3] == 0 || h[3] > 60) return false;
    if (s->size() > (1u << 30)) return false;
    std::vector<uint8_t> d = s->get(0, (size_t)s->size());
    Span D(d);
    // The frame rectangle: 5 bits giving the field width, then four fields.
    uint32_t nbits = d[8] >> 3;
    size_t pos = 8 + (5 + 4 * nbits + 7) / 8 + 4;
    std::vector<uint8_t> tables, stream;
    int found = 0;
    for (int guard = 0; pos + 2 <= d.size() && guard < 1000000; ++guard) {
        uint32_t cl = D.u16(pos);
        uint32_t code = cl >> 6;
        uint64_t len = cl & 0x3F;
        pos += 2;
        if (len == 0x3F) {
            len = D.u32(pos);
            pos += 4;
        }
        if (code == 0 || pos + len > d.size()) break;
        const uint8_t* t = d.data() + pos;
        size_t n = (size_t)len;
        auto jpeg = [&](const uint8_t* p, size_t k, uint32_t id) {
            // Old movies wrap the data in an extra end/start marker pair (FF D9 FF D8); drop it.
            std::vector<uint8_t> v(p, p + k);
            if (v.size() >= 4 && v[0] == 0xFF && v[1] == 0xD9 && v[2] == 0xFF && v[3] == 0xD8) v.erase(v.begin(), v.begin() + 4);
            std::string ext = v.size() >= 8 && v[0] == 0x89 && v[1] == 'P' ? ".png" : v.size() >= 3 && v[0] == 'G' && v[1] == 'I' ? ".gif" : ".jpg";
            ctx.out.items.push_back(builtItem(ctx.prefix + std::to_string(id) + ext, std::move(v), 0));
            ++found;
        };
        if (code == 8 && n) {  // JPEGTables, shared by DefineBits pictures
            tables.assign(t, t + n);
        } else if (code == 6 && n > 2) {  // DefineBits: the tables, then the picture without its own start marker
            std::vector<uint8_t> v = tables;
            if (v.size() >= 2 && v[v.size() - 2] == 0xFF && v[v.size() - 1] == 0xD9) v.resize(v.size() - 2);
            size_t skip = n > 4 && t[2] == 0xFF && t[3] == 0xD8 ? 4 : 2;
            v.insert(v.end(), t + skip, t + n);
            jpeg(v.data(), v.size(), Span(t, n).u16(0));
        } else if (code == 21 && n > 2) {  // DefineBitsJPEG2
            jpeg(t + 2, n - 2, Span(t, n).u16(0));
        } else if ((code == 35 || code == 90) && n > 6) {  // DefineBitsJPEG3/4: picture, then alpha
            uint32_t alphaAt = Span(t, n).u32(2);
            size_t start = code == 35 ? 6 : 8;
            if (start + alphaAt <= n) jpeg(t + start, alphaAt, Span(t, n).u16(0));
        } else if (code == 14 && n > 7 && (t[2] >> 4) == 2) {  // DefineSound with MP3 data (after a 2-byte seek)
            std::vector<uint8_t> v(t + 9, t + n);
            ctx.out.items.push_back(builtItem(ctx.prefix + std::to_string(Span(t, n).u16(0)) + ".mp3", std::move(v), 0));
            ++found;
        } else if (code == 19 && n > 4) {  // SoundStreamBlock: MP3 frames after sample count and seek
            stream.insert(stream.end(), t + 4, t + n);
        }
        pos += n;
    }
    if (!stream.empty()) {
        ctx.out.items.push_back(builtItem(ctx.prefix + "stream.mp3", std::move(stream), 0));
        ++found;
    }
    if (!found) return false;
    ctx.formats.push_back("SWF");
    return true;
}

// =============================================================================================== FLV
struct FlvTrack {
    std::string name;
    uint64_t size = 0;
};

// Walks the tags, handing each output track its bytes in order. Tracks: 0 audio, 1 video.
bool flvWalk(Stream& s, const std::function<bool(int, const uint8_t*, size_t)>& emit, std::string names[2]) {
    std::vector<uint8_t> h = s.getUpTo(0, 9);
    Span H(h);
    uint64_t pos = H.u32(5) + 4;
    std::vector<uint8_t> aacConfig;
    uint32_t nalLen = 4;
    std::vector<uint8_t> tag;
    auto adts = [&](const uint8_t* p, size_t n) {
        // Raw AAC frames get the 7-byte ADTS header players expect, built from the stream's config.
        if (aacConfig.size() < 2) return emit(0, p, n);
        uint32_t profile = (aacConfig[0] >> 3) - 1, rate = (aacConfig[0] & 7) << 1 | aacConfig[1] >> 7,
                 ch = (aacConfig[1] >> 3) & 15, len = (uint32_t)n + 7;
        uint8_t a[7] = {0xFF, 0xF1, (uint8_t)((profile & 3) << 6 | (rate & 15) << 2 | (ch >> 2)),
                        (uint8_t)((ch & 3) << 6 | len >> 11), (uint8_t)(len >> 3), (uint8_t)((len & 7) << 5 | 0x1F), 0xFC};
        return emit(0, a, 7) && emit(0, p, n);
    };
    static const uint8_t start[4] = {0, 0, 0, 1};
    for (int guard = 0; pos + 11 <= s.size() && guard < 50000000; ++guard) {
        uint8_t th[11];
        if (!s.readAt(pos, th, 11)) break;
        uint32_t type = th[0] & 0x1F, len = (uint32_t)th[1] << 16 | th[2] << 8 | th[3];
        if (pos + 11 + len > s.size()) break;
        tag.resize(len);
        if (len && !s.readAt(pos + 11, tag.data(), len)) break;
        pos += 11 + len + 4;
        if (!len) continue;
        if (type == 8) {
            uint32_t fmt = tag[0] >> 4;
            if (fmt == 10) {
                if (len < 2) continue;
                names[0] = "audio.aac";
                if (tag[1] == 0) aacConfig.assign(tag.begin() + 2, tag.end());
                else if (!adts(tag.data() + 2, len - 2)) return false;
            } else {
                static const char* const ext[16] = {"wav", "adpcm", "mp3", "wav", "nelly", "nelly", "nelly", "alaw",
                                                    "ulaw", "bin", "aac", "speex", "bin", "bin", "mp3", "bin"};
                names[0] = std::string("audio.") + ext[fmt];
                if (!emit(0, tag.data() + 1, len - 1)) return false;
            }
        } else if (type == 9) {
            uint32_t codec = tag[0] & 15;
            if ((tag[0] >> 4) == 5) continue;  // an info/command frame, not picture data
            if (codec == 7 || codec == 12) {  // H.264 / H.265: length-prefixed NAL units -> start codes
                names[1] = codec == 7 ? "video.264" : "video.265";
                if (len < 5) continue;
                const uint8_t* p = tag.data() + 5;
                size_t n = len - 5;
                if (tag[1] == 0 && codec == 7 && n >= 7) {  // the decoder configuration: SPS and PPS
                    nalLen = (p[4] & 3) + 1u;
                    size_t i = 6;
                    for (int set = 0; set < 2; ++set) {
                        uint32_t count = set == 0 ? (p[5] & 31) : (i < n ? p[i++] : 0);
                        for (uint32_t k = 0; k < count && i + 2 <= n; ++k) {
                            uint32_t l = p[i] << 8 | p[i + 1];
                            i += 2;
                            if (i + l > n) break;
                            if (!emit(1, start, 4) || !emit(1, p + i, l)) return false;
                            i += l;
                        }
                    }
                } else if (tag[1] == 1) {
                    for (size_t i = 0; i + nalLen <= n;) {
                        uint32_t l = 0;
                        for (uint32_t k = 0; k < nalLen; ++k) l = l << 8 | p[i + k];
                        i += nalLen;
                        if (i + l > n) break;
                        if (!emit(1, start, 4) || !emit(1, p + i, l)) return false;
                        i += l;
                    }
                }
            } else {
                static const char* const ext[16] = {"bin", "bin", "h263", "screen", "vp6", "vp6a", "screen2", "264",
                                                    "bin", "bin", "bin", "bin", "265", "bin", "bin", "bin"};
                names[1] = std::string("video.") + ext[codec];
                // VP6 frames start with a one-byte adjustment (VP6 alpha adds an offset too).
                size_t skip = codec == 4 ? 2 : codec == 5 ? 5 : 1;
                if (len > skip && !emit(1, tag.data() + skip, len - skip)) return false;
            }
        }
    }
    return true;
}

bool openFlv(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> h = s->getUpTo(0, 9);
    if (h.size() < 9 || !Span(h).eq(0, "FLV", 3) || h[3] != 1) return false;
    std::string names[2];
    uint64_t sizes[2] = {0, 0};
    flvWalk(*s, [&](int t, const uint8_t*, size_t n) { return sizes[t] += n, true; }, names);
    ctx.formats.push_back("FLV");
    for (int t = 0; t < 2; ++t) {
        if (!sizes[t]) continue;
        Item it;
        it.path = ctx.prefix + names[t];
        it.size = sizes[t];
        it.read = [s, t](const Sink& sink, std::string& err) {
            std::string n2[2];
            bool stopped = false;
            bool ok = flvWalk(*s, [&](int tt, const uint8_t* p, size_t n) {
                if (tt != t) return true;
                if (!sink(p, n)) return stopped = true, false;
                return true;
            }, n2);
            if (!ok && !stopped) err = "Can't read the video";
            return ok || stopped;
        };
        ctx.out.items.push_back(std::move(it));
    }
    if (ctx.out.items.empty()) ctx.out.warning = "This video has no audio or picture data";
    return true;
}

// ============================================================================================ Base64
int b64(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

bool looksBase64(const std::vector<uint8_t>& h) {
    if (h.size() < 4) return false;
    size_t good = 0;
    for (uint8_t c : h) {
        if (b64(c) >= 0 || c == '=') ++good;
        else if (c != '\r' && c != '\n' && c != ' ' && c != '\t') return false;
    }
    return good >= 4;
}

bool openBase64(Ctx& ctx, StreamPtr s) {
    if (s->size() > (512u << 20)) return false;
    std::vector<uint8_t> in = s->get(0, (size_t)s->size());
    // A MIME-style "begin-base64" or PEM "-----BEGIN" line is skipped, as is its closing line.
    size_t from = 0, to = in.size();
    std::string text(in.begin(), in.begin() + (ptrdiff_t)std::min<size_t>(in.size(), 256));
    if (text.compare(0, 13, "begin-base64 ") == 0 || text.compare(0, 10, "-----BEGIN") == 0) {
        while (from < in.size() && in[from] != '\n') ++from;
    }
    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = from; i < to; ++i) {
        uint8_t c = in[i];
        if (c == '=' || (c == '-' && i + 4 < to && in[i + 1] == '-')) break;  // padding, or an end line
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        int v = b64(c);
        if (v < 0) {
            if (out.empty()) return false;
            ctx.out.warning = "Stopped at a character that isn't base64";
            break;
        }
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)(acc >> bits));
        }
    }
    if (out.empty()) return false;
    ctx.formats.push_back("Base64");
    std::string name = ctx.name.empty() ? "data" : stem(ctx.name);
    if (name == ctx.name || name.empty()) name += ".bin";
    if (name.find('.') == std::string::npos) name += extFor(out.data(), out.size());
    ctx.out.items.push_back(builtItem(ctx.prefix + component(name), std::move(out), 0));
    return true;
}

// ============================================================================================== IHex
int hexv(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool openIhex(Ctx& ctx, StreamPtr s) {
    if (s->size() > (256u << 20)) return false;
    std::vector<uint8_t> in = s->get(0, (size_t)s->size());
    std::map<uint64_t, std::vector<uint8_t>> blocks;  // start address -> contiguous bytes
    uint64_t base = 0;
    size_t i = 0, records = 0;
    bool ended = false;
    std::vector<uint8_t> rec;
    while (i < in.size() && !ended) {
        uint8_t c = in[i++];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        if (c != ':') {
            if (!records) return false;
            ctx.out.warning = "Stopped at a line that isn't a HEX record";
            break;
        }
        rec.clear();
        while (i + 1 < in.size() && hexv(in[i]) >= 0 && hexv(in[i + 1]) >= 0) {
            rec.push_back((uint8_t)(hexv(in[i]) << 4 | hexv(in[i + 1])));
            i += 2;
        }
        if (rec.size() < 5 || rec.size() != rec[0] + 5u) return false;
        uint8_t sum = 0;
        for (uint8_t b : rec) sum += b;
        if (sum != 0) {
            ctx.err = "Damaged HEX file (checksum)";
            return false;
        }
        ++records;
        uint32_t n = rec[0], addr = rec[1] << 8 | rec[2], type = rec[3];
        const uint8_t* data = rec.data() + 4;
        if (type == 0) {
            uint64_t at = base + addr;
            // Join onto the block that ends here, if any.
            auto it = blocks.upper_bound(at);
            if (it != blocks.begin()) {
                auto prev = std::prev(it);
                if (prev->first + prev->second.size() == at) {
                    prev->second.insert(prev->second.end(), data, data + n);
                    continue;
                }
            }
            blocks[at].assign(data, data + n);
        } else if (type == 1) {
            ended = true;
        } else if (type == 2 && n == 2) {
            base = (uint64_t)(data[0] << 8 | data[1]) << 4;
        } else if (type == 4 && n == 2) {
            base = (uint64_t)(data[0] << 8 | data[1]) << 16;
        }
    }
    if (!records || blocks.empty()) return false;
    // Blocks that touch after all (records out of order) are merged.
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> merged;
    for (auto& b : blocks) {
        if (!merged.empty() && merged.back().first + merged.back().second.size() == b.first)
            merged.back().second.insert(merged.back().second.end(), b.second.begin(), b.second.end());
        else
            merged.push_back({b.first, std::move(b.second)});
    }
    ctx.formats.push_back("IHex");
    // Numbered in address order, as 7-Zip numbers them.
    for (size_t k = 0; k < merged.size(); ++k)
        ctx.out.items.push_back(builtItem(ctx.prefix + std::to_string(k), std::move(merged[k].second), 0));
    return true;
}

bool probeIhex(const std::vector<uint8_t>& h, const std::string& name) {
    if (h.size() < 11 || h[0] != ':') return false;
    for (size_t i = 1; i < 11; ++i)
        if (hexv(h[i]) < 0) return false;
    std::string e = extOf(name);
    return e == "hex" || e == "ihx" || e == "ihex" || e == "h86" || e == "a43" || e == "a90" || e == "mcs";
}

}  // namespace

const Handler hMslz = {"MsLZ",
                       [](Stream&, const std::vector<uint8_t>& h, const std::string&) {
                           return Span(h).eq(0, "SZDD\x88\xF0\x27\x33", 8);
                       },
                       openMslz};

const Handler hSwf = {"SWF",
                      [](Stream&, const std::vector<uint8_t>& h, const std::string&) {
                          Span H(h);
                          return h.size() >= 8 && (H.eq(0, "CWS", 3) || H.eq(0, "ZWS", 3) || H.eq(0, "FWS", 3)) && h[3] && h[3] < 60;
                      },
                      [](Ctx& ctx, StreamPtr s) {
                          std::vector<uint8_t> h = s->getUpTo(0, 3);
                          return Span(h).eq(0, "FWS", 3) ? openSwf(ctx, s) : openSwfc(ctx, s);
                      }};

const Handler hFlv = {"FLV",
                      [](Stream&, const std::vector<uint8_t>& h, const std::string&) {
                          return h.size() >= 9 && Span(h).eq(0, "FLV", 3) && h[3] == 1;
                      },
                      openFlv};

const Handler hBase64 = {"Base64",
                         [](Stream&, const std::vector<uint8_t>& h, const std::string& name) {
                             std::string e = extOf(name);
                             if (e != "b64" && e != "base64") return false;
                             std::vector<uint8_t> head(h.begin(), h.begin() + (ptrdiff_t)std::min<size_t>(h.size(), 4096));
                             std::string t(head.begin(), head.begin() + (ptrdiff_t)std::min<size_t>(head.size(), 16));
                             if (t.compare(0, 13, "begin-base64 ") == 0 || t.compare(0, 10, "-----BEGIN") == 0) return true;
                             // Only look at whole lines: the head may end mid-way.
                             while (!head.empty() && head.back() != '\n' && head.size() == 4096) head.pop_back();
                             return looksBase64(head);
                         },
                         openBase64};

const Handler hIhex = {"IHex",
                       [](Stream&, const std::vector<uint8_t>& h, const std::string& name) { return probeIhex(h, name); },
                       openIhex};

}  // namespace kz::native
