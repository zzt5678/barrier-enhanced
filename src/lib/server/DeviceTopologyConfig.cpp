#include "DeviceTopologyConfig.h"
#include "server/Config.h"

namespace weave { namespace topology {
Topology topologyFromConfig(const Config& config,const std::string& primary,
    const std::set<std::string>& admitted,std::uint64_t epoch,std::uint64_t revision) {
    Topology t; t.epoch=epoch; t.revision=revision;
    t.viewerName=config.getCanonicalName(primary);
    auto invalid=[&] {
        t.nodes.clear(); t.layout=Layout::InvalidConfiguration; return t;
    };
    if (!validName(t.viewerName)) return invalid();
    std::set<std::string> online;
    for (const auto& name:admitted) online.insert(config.getCanonicalName(name));
    std::vector<Edge> edges;
    for (auto i=config.begin();i!=config.end();++i) {
        const auto name=*i;
        if (t.nodes.size()==MaxNodes || !validName(name)) return invalid();
        const bool server=name==t.viewerName;
        t.nodes.push_back({name,server?Role::Server:Role::Client,
                           server||online.count(name)!=0,0,0});
        for (auto e=config.beginNeighbor(name);e!=config.endNeighbor(name);++e) {
            if (edges.size()==MaxNodes*4) return invalid();
            const auto a=e->first.getInterval(), b=e->second.getInterval();
            const auto side=e->first.getSide();
            edges.push_back({name,config.getCanonicalName(e->second.getName()),
                side==kLeft?-1:side==kRight?1:0,
                side==kTop?-1:side==kBottom?1:0,
                a.first==0 && a.second==1 && b.first==0 && b.second==1});
        }
    }
    resolveLayout(t,edges);
    std::string validation;
    if (!encodeTopology(t,validation)) return invalid();
    return t;
}
} }
