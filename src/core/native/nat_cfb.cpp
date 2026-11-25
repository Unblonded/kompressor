// Compound File Binary (OLE2): MSI/MSP/MSM installers, old Office documents, Outlook messages.
#include <algorithm>

#include "nat.hpp"

namespace kz::native {

namespace {

// ======================================================================================= Compound File
constexpr uint32_t kEnd = 0xFFFFFFFA;  // sector ids from here up are markers (end of chain, free...)

// MSI packs table and stream names: two name characters per UTF-16 unit from a 64-letter alphabet, and
// a marker in front of table names (shown as '!', as 7-Zip does). Plain names pass through.
std::string streamName(const uint8_t* p, size_t chars, bool& msi) {
    static const char kChars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._";
    std::string s;
    for (size_t i = 0; i < chars; ++i) {
        uint32_t c = (uint32_t)p[i * 2] | (uint32_t)p[i * 2 + 1] << 8;
        if (c >= 0x3800 && c < 0x4840) {
            c -= 0x3800;
            s += kChars[c & 63];
            if ((c >> 6) < 64) s += kChars[c >> 6];
            msi = true;
        } else if (c == 0x4840) {
            s += '!';
            msi = true;
        } else if (c < 0x20) {
            s += "[" + std::to_string(c) + "]";  // "\5SummaryInformation" -> "[5]SummaryInformation"
        } else {
            s += utf16le(p + i * 2, 1);
        }
    }
    return s;
}

bool openCfb(StreamPtr sp, std::vector<Item>& items, std::string& format, std::string& err) {
    Stream& b = *sp;
    std::vector<uint8_t> hv = b.get(0, 512);
    Span H(hv);
    if (hv.empty() || std::memcmp(hv.data(), "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) != 0) {
        err = "Not a Compound File";
        return false;
    }
    uint32_t shift = H.u16(0x1E), mshift = H.u16(0x20), major = H.u16(0x1A);
    if ((shift != 9 && shift != 12) || mshift != 6) {
        err = "Damaged Compound File (bad sector size)";
        return false;
    }
    const uint32_t ssz = 1u << shift, msz = 1u << mshift;
    uint32_t nFat = H.u32(0x2C), dir1 = H.u32(0x30), cutoff = H.u32(0x38), mf1 = H.u32(0x3C), dif1 = H.u32(0x44),
             nDif = H.u32(0x48);
    auto secOff = [&](uint32_t s) { return (uint64_t)(s + 1) << shift; };
    uint64_t maxSecs = b.size() / ssz + 1;
    if (nFat > maxSecs) {
        err = "Damaged Compound File (bad allocation table size)";
        return false;
    }

    // The allocation table: its sectors are listed in the header and then in a chain of DIFAT sectors.
    std::vector<uint32_t> fatSecs;
    for (uint32_t i = 0; i < 109 && fatSecs.size() < nFat; ++i) {
        uint32_t v = H.u32(0x4C + i * 4);
        if (v < kEnd) fatSecs.push_back(v);
    }
    uint32_t d = dif1;
    for (uint32_t k = 0; k < nDif && d < kEnd && fatSecs.size() < nFat; ++k) {
        std::vector<uint8_t> sv = b.get(secOff(d), ssz);
        if (sv.empty()) break;
        Span S(sv);
        uint32_t per = ssz / 4 - 1;
        for (uint32_t j = 0; j < per && fatSecs.size() < nFat; ++j) {
            uint32_t v = S.u32(j * 4);
            if (v < kEnd) fatSecs.push_back(v);
        }
        d = S.u32(per * 4);
    }
    std::vector<uint32_t> fat;
    fat.reserve(fatSecs.size() * (ssz / 4));
    for (uint32_t s : fatSecs) {
        std::vector<uint8_t> sv = b.get(secOff(s), ssz);
        if (sv.empty()) {
            err = "Damaged Compound File (allocation table outside the file)";
            return false;
        }
        Span S(sv);
        for (uint32_t j = 0; j < ssz / 4; ++j) fat.push_back(S.u32(j * 4));
    }
    auto chain = [&](uint32_t s) {
        std::vector<uint32_t> c;
        while (s < kEnd && s < fat.size() && c.size() <= fat.size()) {
            c.push_back(s);
            s = fat[s];
        }
        return c;
    };

    // The directory: 128-byte entries forming a tree.
    std::vector<uint8_t> dir;
    for (uint32_t s : chain(dir1)) {
        std::vector<uint8_t> sv = b.get(secOff(s), ssz);
        if (sv.empty()) break;
        dir.insert(dir.end(), sv.begin(), sv.end());
    }
    Span D(dir);
    size_t nEnt = dir.size() / 128;
    if (nEnt == 0 || D.u8(0x42) != 5) {
        err = "Damaged Compound File (no root entry)";
        return false;
    }
    // Small streams live in the "mini stream" (the root entry's data), 64 bytes at a time.
    std::vector<uint32_t> minifat;
    for (uint32_t s : chain(mf1)) {
        std::vector<uint8_t> sv = b.get(secOff(s), ssz);
        if (sv.empty()) break;
        Span S(sv);
        for (uint32_t j = 0; j < ssz / 4; ++j) minifat.push_back(S.u32(j * 4));
    }
    std::vector<uint32_t> rootChain = chain(D.u32(0x74));
    int64_t rootTime = fromFiletime(D.u64(0x6C));

    auto entrySize = [&](size_t i) { return major == 3 ? (uint64_t)D.u32(i * 128 + 0x78) : D.u64(i * 128 + 0x78); };
    bool msi = false;
    std::vector<bool> seen(nEnt, false);
    std::vector<std::pair<uint32_t, std::string>> todo;  // (entry, folder path)
    todo.emplace_back(D.u32(0x4C), "");
    while (!todo.empty()) {
        auto [e, prefix] = todo.back();
        todo.pop_back();
        if (e >= nEnt || seen[e]) continue;
        seen[e] = true;
        size_t o = (size_t)e * 128;
        todo.emplace_back(D.u32(o + 0x44), prefix);  // left and right siblings
        todo.emplace_back(D.u32(o + 0x48), prefix);
        uint32_t type = D.u8(o + 0x42), nameBytes = std::min<uint32_t>(D.u16(o + 0x40), 64);
        size_t chars = nameBytes >= 2 ? nameBytes / 2 - 1 : 0;
        std::string name = component(streamName(dir.data() + o, chars, msi));
        if (type == 1) {  // storage: a folder
            todo.emplace_back(D.u32(o + 0x4C), prefix + name + "/");
            continue;
        }
        if (type != 2) continue;
        std::vector<std::pair<uint64_t, uint64_t>> runs;
        std::string path = prefix + name;
        int64_t mt = fromFiletime(D.u64(o + 0x6C));
        uint64_t size = entrySize(e), left = size;
        uint32_t s = D.u32(o + 0x74);
        if (size < cutoff) {
            for (size_t guard = 0; left && s < kEnd && s < minifat.size() && guard <= minifat.size(); ++guard) {
                uint64_t pos = (uint64_t)s << mshift;
                uint64_t idx = pos >> shift;
                if (idx >= rootChain.size()) break;
                uint64_t len = std::min<uint64_t>(msz, left);
                addRun(runs, secOff(rootChain[idx]) + (pos & (ssz - 1)), len);
                left -= len;
                s = minifat[s];
            }
        } else {
            for (size_t guard = 0; left && s < kEnd && s < fat.size() && guard <= fat.size(); ++guard) {
                uint64_t len = std::min<uint64_t>(ssz, left);
                addRun(runs, secOff(s), len);
                left -= len;
                s = fat[s];
            }
        }
        // An installer's cabinets are often stored without an extension: give them one so they open.
        if (name.find('.') == std::string::npos && !runs.empty()) {
            std::vector<uint8_t> h = b.getUpTo(runs[0].first, (size_t)std::min<uint64_t>(runs[0].second, 8));
            if (h.size() == 8 && std::memcmp(h.data(), "MSCF\0\0\0\0", 8) == 0) path += ".cab";
        }
        // A broken chain: what could be found.
        items.push_back(runsItem(path, sp, std::move(runs), size - left, mt ? mt : rootTime));
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.path < b.path; });
    if (items.empty()) {
        err = "This Compound File holds no streams";
        return false;
    }
    format = msi ? "MSI installer" : "Compound File";
    return true;
}

}  // namespace

const Handler hCfb = {
    "Compound",
    [](Stream&, const std::vector<uint8_t>& h, const std::string&) {
        return h.size() >= 512 && std::memcmp(h.data(), "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) == 0;
    },
    [](Ctx& ctx, StreamPtr s) {
        std::vector<Item> items;
        std::string format;
        if (!openCfb(s, items, format, ctx.err)) return false;
        for (auto& it : items) {
            it.path = ctx.prefix + it.path;
            ctx.out.items.push_back(std::move(it));
        }
        ctx.formats.push_back(format);
        return true;
    },
};

}  // namespace kz::native
