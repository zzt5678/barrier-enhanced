#include "DeviceTopology.h"
#include "ByteCodec.h"
#include <algorithm>
#include <map>
#include <set>
#include <cstdlib>

namespace weave { namespace topology {
namespace {
std::string folded(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
bool valid(const Topology& t) {
    if (t.nodes.size() > MaxNodes || t.epoch == 0 ||
        unsigned(t.layout) > unsigned(Layout::Unavailable)) return false;
    if (t.nodes.empty()) return (t.layout == Layout::InvalidConfiguration ||
        t.layout == Layout::Unavailable) &&
        (t.viewerName.empty() || validName(t.viewerName));
    std::set<std::string> names;
    std::set<std::pair<int,int>> positions;
    unsigned servers = 0;
    bool viewer = t.viewerName.empty();
    for (const auto& n : t.nodes) {
        if (!validName(n.name) || !names.insert(folded(n.name)).second ||
            unsigned(n.role) > unsigned(Role::Client) ||
            n.x < -63 || n.x > 63 || n.y < -63 || n.y > 63) return false;
        if (n.role == Role::Server) {
            ++servers;
            if (t.layout == Layout::Available && (n.x || n.y)) return false;
        }
        if (n.name == t.viewerName) viewer = true;
        if (t.layout == Layout::Available && !positions.insert({n.x,n.y}).second)
            return false;
        if (t.layout != Layout::Available && (n.x || n.y)) return false;
    }
    return servers == 1 && viewer;
}
}
bool validName(const std::string& s) {
    if (s.empty() || s.size() > MaxNameBytes) return false;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned c = static_cast<unsigned char>(s[i++]);
        if (c < 0x20 || c == 0x7f) return false;
        if (c < 0x80) continue;
        unsigned extra = 0, cp = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { extra = 1; cp = c & 31; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { extra = 2; cp = c & 15; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { extra = 3; cp = c & 7; minimum = 0x10000; }
        else return false;
        if (extra > s.size() - i) return false;
        while (extra--) {
            const unsigned b = static_cast<unsigned char>(s[i++]);
            if ((b & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (b & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
    }
    return true;
}
bool encodeTopology(const Topology& t, std::string& out) {
    if (!valid(t)) return false;
    std::string s("WTP1");
    detail::number(s, t.epoch, 8); detail::number(s, t.revision, 8);
    detail::string(s, t.viewerName);
    detail::number(s, unsigned(t.layout), 1); detail::number(s, t.nodes.size(), 1);
    for (const auto& n : t.nodes) {
        detail::string(s, n.name); detail::number(s, unsigned(n.role), 1);
        detail::number(s, n.online, 1);
        detail::number(s, n.x + 63, 1); detail::number(s, n.y + 63, 1);
    }
    if (s.size() > MaxBytes) return false;
    out = std::move(s); return true;
}
bool decodeTopology(const std::string& s, Topology& out) {
    if (s.size() > MaxBytes || s.size() < 24 || s.compare(0,4,"WTP1")) return false;
    detail::Reader r{s}; r.p = 4;
    Topology t; t.epoch = r.number(8); t.revision = r.number(8);
    t.viewerName = r.string(MaxNameBytes); t.layout = Layout(r.number(1));
    const auto count = r.number(1);
    if (!r.good || count > MaxNodes) return false;
    for (unsigned i = 0; i < count; ++i) {
        Node n; n.name = r.string(MaxNameBytes); n.role = Role(r.number(1));
        const auto online = r.number(1);
        if (online > 1) return false;
        n.online = online; n.x = int(r.number(1)) - 63; n.y = int(r.number(1)) - 63;
        if (!r.good) return false;
        t.nodes.push_back(std::move(n));
    }
    if (!r.done() || !valid(t)) return false;
    out = std::move(t); return true;
}
void resolveLayout(Topology& t, const std::vector<Edge>& edges) {
    for (auto& n : t.nodes) n.x = n.y = 0;
    t.layout = Layout::Unavailable;
    if (!valid(t)) { t.layout = Layout::InvalidConfiguration; return; }
    std::map<std::string, std::size_t> index;
    std::vector<bool> placed(t.nodes.size(), false);
    for (std::size_t i = 0; i < t.nodes.size(); ++i) {
        index[t.nodes[i].name] = i;
        if (t.nodes[i].role == Role::Server) placed[i] = true;
    }
    auto fail = [&](Layout reason) {
        t.layout = reason;
        for (auto& n : t.nodes) n.x = n.y = 0;
    };
    if (edges.size() > MaxNodes * 4) { fail(Layout::InvalidConfiguration); return; }
    for (const auto& e : edges) {
        if (!e.whole) { fail(Layout::PartialEdge); return; }
        if (!index.count(e.from) || !index.count(e.to) ||
            !((e.dx == -1 && e.dy == 0) || (e.dx == 1 && e.dy == 0) ||
              (e.dx == 0 && e.dy == -1) || (e.dx == 0 && e.dy == 1))) {
            fail(Layout::InvalidConfiguration); return;
        }
    }
    for (std::size_t pass = 0; pass < t.nodes.size(); ++pass) {
        for (const auto& e : edges) {
            const auto a = index[e.from], b = index[e.to];
            if (!placed[a] && !placed[b]) continue;
            auto& from = t.nodes[a]; auto& to = t.nodes[b];
            if (placed[a] && placed[b]) {
                if (to.x != from.x + e.dx || to.y != from.y + e.dy) {
                    fail(Layout::Contradictory); return;
                }
            } else if (placed[a]) {
                to.x = from.x + e.dx; to.y = from.y + e.dy; placed[b] = true;
            } else {
                from.x = to.x - e.dx; from.y = to.y - e.dy; placed[a] = true;
            }
        }
    }
    if (std::find(placed.begin(), placed.end(), false) != placed.end()) {
        fail(Layout::Disconnected); return;
    }
    t.layout = Layout::Available;
    if (!valid(t)) fail(Layout::Contradictory);
}
} }
