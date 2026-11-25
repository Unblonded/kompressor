// Volume managers that carve one disk into named volumes:
//   LVM2   - Linux logical volumes: a "LABELONE" label, then a text description of the volume group.
//   LP     - Android dynamic partitions (super.img): a geometry block and a table of named partitions.
// Each volume is explored (ext4, NTFS...) as a folder named after it.
#include <algorithm>
#include <cctype>
#include <map>

#include "nat.hpp"

namespace kz::native {

namespace {

// ============================================================================================== LVM2
// The metadata text: "name { key = value ... }" blocks; values are numbers, "strings" or [lists].
struct Conf {
    std::map<std::string, std::string> values;
    std::map<std::string, std::vector<std::string>> lists;
    std::vector<std::pair<std::string, Conf>> blocks;
    const Conf* block(const std::string& n) const {
        for (auto& b : blocks)
            if (b.first == n) return &b.second;
        return nullptr;
    }
    uint64_t num(const std::string& k, uint64_t def = 0) const {
        auto it = values.find(k);
        return it == values.end() ? def : std::strtoull(it->second.c_str(), nullptr, 10);
    }
    std::string str(const std::string& k) const {
        auto it = values.find(k);
        return it == values.end() ? "" : it->second;
    }
};

struct ConfParser {
    const std::string& t;
    size_t i = 0;
    void space() {
        while (i < t.size()) {
            if (std::isspace((unsigned char)t[i])) ++i;
            else if (t[i] == '#') while (i < t.size() && t[i] != '\n') ++i;
            else break;
        }
    }
    std::string word() {
        space();
        size_t a = i;
        while (i < t.size() && (std::isalnum((unsigned char)t[i]) || t[i] == '_' || t[i] == '-' || t[i] == '.')) ++i;
        return t.substr(a, i - a);
    }
    std::string value() {
        space();
        if (i < t.size() && t[i] == '"') {
            size_t a = ++i;
            while (i < t.size() && t[i] != '"') ++i;
            std::string v = t.substr(a, i - a);
            if (i < t.size()) ++i;
            return v;
        }
        return word();
    }
    void parse(Conf& c, int depth) {
        while (depth < 32) {
            space();
            if (i >= t.size()) return;
            if (t[i] == '}') {
                ++i;
                return;
            }
            std::string k = word();
            if (k.empty()) {
                ++i;
                continue;
            }
            space();
            if (i < t.size() && t[i] == '{') {
                ++i;
                c.blocks.push_back({k, Conf()});
                parse(c.blocks.back().second, depth + 1);
            } else if (i < t.size() && t[i] == '=') {
                ++i;
                space();
                if (i < t.size() && t[i] == '[') {
                    ++i;
                    std::vector<std::string> l;
                    for (int g = 0; g < 100000; ++g) {
                        space();
                        if (i >= t.size() || t[i] == ']') break;
                        if (t[i] == ',') {
                            ++i;
                            continue;
                        }
                        l.push_back(value());
                    }
                    if (i < t.size()) ++i;
                    c.lists[k] = l;
                } else {
                    c.values[k] = value();
                }
            }
        }
    }
};

bool openLvm(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> h = s->getUpTo(0, 2048);
    size_t lab = SIZE_MAX;
    for (size_t o = 0; o + 32 <= h.size(); o += 512)
        if (Span(h).eq(o, "LABELONE", 8) && Span(h).eq(o + 24, "LVM2 001", 8)) lab = o;
    if (lab == SIZE_MAX) return false;
    Span H(h);
    size_t pvh = lab + H.u32(lab + 20);
    std::vector<uint8_t> pv = s->getUpTo(pvh, 4096);
    Span P(pv);
    std::string pvId((const char*)pv.data(), std::min<size_t>(32, pv.size()));
    // Data areas, then metadata areas: (offset, size) pairs, each list ending with zeros.
    size_t o = 40;
    while (P.has(o, 16) && P.u64(o)) o += 16;
    o += 16;
    uint64_t mda = P.u64(o);
    if (!mda) {
        ctx.err = "This LVM disk has no metadata copy";
        return false;
    }
    std::vector<uint8_t> mh = s->getUpTo(mda, 512);
    Span M(mh);
    if (!M.eq(4, " LVM2 x[5A%r0N*>", 16)) {
        ctx.err = "Damaged LVM metadata";
        return false;
    }
    uint64_t start = M.u64(24), size = M.u64(32), at = M.u64(40), len = M.u64(48);
    if (at >= size || len > size || len > (16u << 20)) return false;
    std::vector<uint8_t> text = s->getUpTo(start + at, (size_t)len);
    if (at + len > size) {  // the circular buffer wrapped
        std::vector<uint8_t> rest = s->getUpTo(start + 512, (size_t)(at + len - size));
        text.resize((size_t)(size - at));
        text.insert(text.end(), rest.begin(), rest.end());
    }
    std::string t(text.begin(), text.end());
    while (!t.empty() && t.back() == 0) t.pop_back();
    Conf root;
    ConfParser cp{t};
    cp.parse(root, 0);
    if (root.blocks.empty()) return false;
    const Conf& vg = root.blocks[0].second;
    uint64_t extent = vg.num("extent_size") * 512;
    const Conf* pvs = vg.block("physical_volumes");
    const Conf* lvs = vg.block("logical_volumes");
    if (!extent || !pvs || !lvs) return false;
    // Which pvN is this disk: ids match without their dashes.
    std::string self;
    uint64_t peStart = 0;
    for (auto& [name, c] : pvs->blocks) {
        std::string id = c.str("id");
        id.erase(std::remove(id.begin(), id.end(), '-'), id.end());
        if (id == pvId || pvs->blocks.size() == 1) self = name, peStart = c.num("pe_start") * 512;
    }
    ctx.formats.push_back("LVM");
    for (auto& [lvName, lv] : lvs->blocks) {
        std::vector<ExtentStream::Extent> ext;
        uint64_t lvSize = 0;
        bool complete = true;
        for (auto& [segName, seg] : lv.blocks) {
            if (segName.compare(0, 7, "segment") != 0) continue;
            uint64_t se = seg.num("start_extent"), cnt = seg.num("extent_count");
            lvSize = std::max(lvSize, (se + cnt) * extent);
            auto st = seg.lists.find("stripes");
            if (seg.str("type") != "striped" || seg.num("stripe_count", 1) != 1 || st == seg.lists.end() || st->second.size() < 2 ||
                st->second[0] != self) {
                complete = false;  // mirrors, thin pools, other disks...
                continue;
            }
            uint64_t pe = std::strtoull(st->second[1].c_str(), nullptr, 10);
            ext.push_back({se * extent, cnt * extent, s, peStart + pe * extent, {}});
        }
        if (!lvSize) continue;
        if (!complete) ctx.out.warning = "Some volumes use parts of other disks or RAID layouts, which read as zeros";
        auto vol = std::make_shared<ExtentStream>(lvSize, std::move(ext));
        explorePart(ctx, vol, component(lvName), component(lvName) + ".img");
    }
    return true;
}

// =============================================================================================== LP
bool openLp(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> g = s->getUpTo(4096, 64);
    Span G(g);
    if (G.u32(0) != 0x616C4467) return false;
    uint32_t maxSize = G.u32(40);
    std::vector<uint8_t> m = s->getUpTo(4096 * 3, (size_t)std::min<uint32_t>(maxSize ? maxSize : 65536, 1u << 20));
    Span M(m);
    if (M.u32(0) != 0x414C5030) {
        ctx.err = "Damaged Android super image (no partition table)";
        return false;
    }
    uint32_t hsize = M.u32(8);
    auto table = [&](size_t o, uint32_t& off, uint32_t& n, uint32_t& es) { off = hsize + M.u32(o), n = M.u32(o + 4), es = M.u32(o + 8); };
    uint32_t po, pn, pes, eo, en, ees;
    table(80, po, pn, pes);
    table(92, eo, en, ees);
    if (pes < 52 || ees < 24 || !M.has(po, (size_t)pn * pes) || !M.has(eo, (size_t)en * ees)) {
        ctx.err = "Damaged Android super image";
        return false;
    }
    ctx.formats.push_back("Android LP");
    for (uint32_t i = 0; i < pn; ++i) {
        size_t p = po + (size_t)i * pes;
        std::string name = cstr(m.data() + p, 36);
        uint32_t first = M.u32(p + 40), count = M.u32(p + 44);
        std::vector<ExtentStream::Extent> ext;
        uint64_t at = 0;
        for (uint32_t k = 0; k < count && first + k < en; ++k) {
            size_t e = eo + (size_t)(first + k) * ees;
            uint64_t sectors = M.u64(e), target = M.u64(e + 12);
            uint32_t type = M.u32(e + 8), dev = M.u32(e + 20);
            if (type == 0 && dev == 0) ext.push_back({at, sectors * 512, s, target * 512, {}});
            at += sectors * 512;
        }
        if (!at || name.empty()) continue;  // an empty slot (the inactive A/B copy)
        explorePart(ctx, std::make_shared<ExtentStream>(at, std::move(ext)), component(name), component(name) + ".img");
    }
    return true;
}

}  // namespace

const Handler hLvm = {"LVM",
                      [](Stream&, const std::vector<uint8_t>& h, const std::string&) {
                          for (size_t o = 0; o + 32 <= std::min<size_t>(h.size(), 2048); o += 512)
                              if (Span(h).eq(o, "LABELONE", 8) && Span(h).eq(o + 24, "LVM2 001", 8)) return true;
                          return false;
                      },
                      openLvm};

const Handler hLp = {"Android LP",
                     [](Stream&, const std::vector<uint8_t>& h, const std::string&) { return Span(h).u32(4096) == 0x616C4467; },
                     openLp};

}  // namespace kz::native
