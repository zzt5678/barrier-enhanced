#pragma once
#include "DeviceTopology.h"
#include <memory>

namespace weave { namespace topology {
constexpr std::uint64_t FreshnessMillis = 6000;
struct ProcessIdentity {
    std::uint64_t pid = 0, birth = 0;
    std::string owner;
    std::uint32_t session = 0;
};
bool operator==(const ProcessIdentity&, const ProcessIdentity&);
struct Snapshot {
    ProcessIdentity identity;
    std::uint64_t run = 0, sequence = 0, updatedMillis = 0;
    // Filled only by readPublishedSnapshot, from the protected daemon proof.
    std::uint64_t ownerGeneration = 0;
    std::string localName;
    Role localRole = Role::Client;
    bool ready = false;
    Topology topology;
};
enum class ReadStatus { Fresh, Stale, NotReady, Missing, Invalid, WrongOwner };
std::uint64_t monotonicMillis();
std::uint64_t newRunId();
bool processIdentity(std::uint64_t pid, ProcessIdentity&);
ProcessIdentity currentProcessIdentity();
std::string privateSnapshotDirectory();
std::string snapshotFilename(const ProcessIdentity&);
bool encodeSnapshot(const Snapshot&, std::string&);
bool decodeSnapshot(const std::string&, Snapshot&);
ReadStatus classifySnapshot(const Snapshot&, const ProcessIdentity&, std::uint64_t now);
// Does not enumerate or choose processes. expected must come from the GUI's own
// QProcess (query birth immediately after launch), or the protected owner proof.
// Stale/NotReady returns historical content; NEVER display its online bits live.
// Malformed, missing and wrong-owner results leave out unchanged.
ReadStatus readSnapshot(const std::string& directory, const ProcessIdentity& expected,
                        std::uint64_t now, Snapshot& out);
class SnapshotPublisher {
public:
    explicit SnapshotPublisher(const std::string& directory = privateSnapshotDirectory());
    ~SnapshotPublisher();
    SnapshotPublisher(const SnapshotPublisher&) = delete;
    SnapshotPublisher& operator=(const SnapshotPublisher&) = delete;
    // Copies bounded metadata only. Disk I/O and 2s liveness publication occur
    // on the worker, never on the input event thread.
    void update(const Topology&, const std::string& localName, Role, bool ready);
private:
    struct State;
    std::unique_ptr<State> m_state;
};

struct PublishedOwner {
    ProcessIdentity identity;
    std::uint64_t generation = 0, updatedMillis = 0;
    // Authenticated launch profile owner; an elevated input core itself may
    // have the SYSTEM token. This field is never inferred from that token.
    std::string launchOwner;
};
bool encodePublishedRecord(const PublishedOwner&, const Snapshot&, std::string&);
bool decodePublishedRecord(const std::string&, PublishedOwner&, Snapshot&);
bool matchesPublishedOwner(const PublishedOwner&, const Snapshot&,
                           const std::string& expectedOwner,
                           std::uint32_t expectedSession, std::uint64_t now);
// Windows only; directory is a fixed child of the native common application
// data directory. Writers require SYSTEM. Readers check owner, protected ACL,
// reparse points, exact process identity, generation and fresh proof twice.
// The daemon reads the exact core snapshot using its token-derived profile
// and publishes a single bounded owner+snapshot record, preserving event time.
std::string publishedOwnerDirectory();
bool publishOwner(const PublishedOwner&);
bool samePublishedGeneration(const PublishedOwner&, const PublishedOwner&);
bool retirePublishedOwner(const PublishedOwner& expected);
ReadStatus readPublishedSnapshot(const std::string& expectedOwner,
                                 std::uint32_t expectedSession,
                                 std::uint64_t now, Snapshot& out);
} }
