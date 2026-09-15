#pragma once

#include "WorkspaceDashboard.h"
#include "topology/TopologySnapshot.h"

struct RuntimeTopologyFallback {
    QVector<WorkspaceDevice> devices;
    QString localName;
    QString serverAddress;
    bool server = false;
    bool editable = false;
    bool spatial = false;
};

struct RuntimeTopologyView {
    QVector<WorkspaceDevice> devices;
    QString status;
    QString layoutReason;
    bool spatial = false;
    bool canArrange = false;
    bool historical = false;
};

// Presentation only. Callers select the own-child or protected published reader.
// Pass evidence only for the reader's Fresh, Stale or NotReady result.
class RuntimeTopologyAdapter {
public:
    RuntimeTopologyView apply(weave::topology::ReadStatus,
        const weave::topology::Snapshot* evidence, std::uint64_t now,
        const RuntimeTopologyFallback&);
    void reset();
private:
    weave::topology::Snapshot m_last;
    bool m_hasLast = false;
};
