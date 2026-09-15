#include "RuntimeTopologyAdapter.h"
#include <QCoreApplication>

namespace {
using namespace weave::topology;
QString text(const char* source) { return QCoreApplication::translate("RuntimeTopologyAdapter", source); }
WorkspaceDeviceRole role(Role value) {
    return value == Role::Server ? WorkspaceDeviceRole::Server : WorkspaceDeviceRole::Client;
}
QString layoutReason(Layout value) {
    switch (value) {
    case Layout::Disconnected: return text("Some devices have no layout links");
    case Layout::PartialEdge: return text("Partial screen links cannot be drawn as a grid");
    case Layout::Contradictory: return text("Layout links conflict");
    case Layout::InvalidConfiguration: return text("The configured layout is invalid");
    case Layout::Unavailable: return text("Waiting for layout coordinates");
    default: return {};
    }
}
}

QString runtimeClipboardSharingState(bool running, bool fresh, bool server, int launchSetting)
{
    if (!running) return text("Paused");
    if (!fresh) return text("Awaiting runtime state");
    if (!server) return text("Managed by Server");
    if (launchSetting == 1) return text("Enabled in running configuration");
    if (launchSetting == 0) return text("Disabled in running configuration");
    return text("Managed by server configuration");
}

RuntimeTopologyView RuntimeTopologyAdapter::apply(ReadStatus status,
    const Snapshot* evidence, std::uint64_t now,
    const RuntimeTopologyFallback& fallback)
{
    RuntimeTopologyView view;
    const bool readable = status == ReadStatus::Fresh || status == ReadStatus::Stale || status == ReadStatus::NotReady;
    if (readable && evidence) {
        std::string checked;
        if (encodeSnapshot(*evidence, checked)) {
            // Replacing the entire record also drops a predecessor's topology on
            // new process/run/server epochs, including empty NotReady records.
            m_last = *evidence;
            m_hasLast = true;
        } else status = ReadStatus::Invalid;
    } else if (readable) status = ReadStatus::Missing;

    if (!m_hasLast) {
        view.devices = fallback.devices;
        if (!fallback.server || view.devices.isEmpty())
            view.devices = {{fallback.localName, {}, true, fallback.server
                ? WorkspaceDeviceRole::Server : WorkspaceDeviceRole::Client}};
        for (auto& device : view.devices) device.state = WorkspaceDeviceState::Configured;
        view.spatial = fallback.server && fallback.spatial;
        view.canArrange = fallback.server && fallback.editable;
        view.status = text("Configured devices · awaiting runtime state");
        if (!fallback.server && !fallback.serverAddress.isEmpty())
            view.status += "\n" + text("Server: %1").arg(fallback.serverAddress);
        if (!view.spatial) view.layoutReason = text("Waiting for layout coordinates");
        return view;
    }

    const auto freshness = classifySnapshot(m_last, m_last.identity, now);
    const bool live = status == ReadStatus::Fresh && freshness == ReadStatus::Fresh;
    view.historical = !live && !m_last.topology.nodes.empty();
    view.status = live ? text("Live device state") : view.historical
        ? text("Historical layout · runtime state unavailable") : text("Awaiting topology from Server");
    view.spatial = m_last.topology.layout == Layout::Available && !m_last.topology.nodes.empty();
    view.layoutReason = layoutReason(m_last.topology.layout);
    const std::string& localName = m_last.topology.viewerName.empty()
        ? m_last.localName : m_last.topology.viewerName;
    bool localServer = false;
    for (const auto& node : m_last.topology.nodes) {
        const bool local = node.name == localName;
        view.devices.append({QString::fromStdString(node.name), QPoint(node.x,node.y), local, role(node.role),
            live ? (node.online ? WorkspaceDeviceState::Connected : WorkspaceDeviceState::Disconnected)
                 : WorkspaceDeviceState::Stale});
        if (local) localServer = node.role == Role::Server;
    }
    if (view.devices.isEmpty()) {
        view.devices.append({QString::fromStdString(m_last.localName), {}, true,
            role(m_last.localRole), freshness == ReadStatus::Stale
                ? WorkspaceDeviceState::Stale : WorkspaceDeviceState::AwaitingSync});
        localServer = m_last.localRole == Role::Server;
        if (!localServer && !fallback.serverAddress.isEmpty())
            view.status += "\n" + text("Server: %1").arg(fallback.serverAddress);
    }
    view.canArrange = localServer && fallback.server && fallback.editable;
    return view;
}

void RuntimeTopologyAdapter::reset() { m_hasLast = false; m_last = {}; }
