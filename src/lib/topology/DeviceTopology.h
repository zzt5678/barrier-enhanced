#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace weave { namespace topology {
constexpr std::size_t MaxBytes = 65536;
constexpr std::size_t MaxNodes = 64;
constexpr std::size_t MaxNameBytes = 256;
enum class Role : std::uint8_t { Server, Client };
enum class Layout : std::uint8_t {
    Available, Disconnected, PartialEdge, Contradictory, InvalidConfiguration,
    Unavailable
};
struct Node {
    std::string name;
    Role role = Role::Client;
    bool online = false;
    int x = 0, y = 0;
};
struct Topology {
    std::uint64_t epoch = 1, revision = 0;
    // Canonical recipient name, resolved by the server (including aliases).
    std::string viewerName;
    Layout layout = Layout::Unavailable;
    std::vector<Node> nodes;
};
struct Edge {
    std::string from, to;
    int dx, dy;
    bool whole;
};
bool validName(const std::string&);
bool encodeTopology(const Topology&, std::string&);
bool decodeTopology(const std::string&, Topology&);
// Directed whole-edge constraints, with the server anchored at (0,0).
// An unavailable result zeroes all coordinates; these must not be displayed.
void resolveLayout(Topology&, const std::vector<Edge>&);
} }
