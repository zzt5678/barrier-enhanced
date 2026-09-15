// Non-installed, read-only native validation companion for the GUI reader.
#include "topology/TopologySnapshot.h"
#include <iomanip>
#include <iostream>
#include <stdexcept>

using namespace weave::topology;
namespace {
std::uint64_t decimal(const char* value) {
    std::string s(value); std::size_t end=0;
    if (s.empty() || s.find_first_not_of("0123456789")!=std::string::npos)
        throw std::runtime_error("invalid identity number");
    auto n=std::stoull(s,&end); if (!n || end!=s.size()) throw std::runtime_error("invalid identity number");
    return n;
}
void identity(const ProcessIdentity& p) {
    std::cout<<"\"pid\":"<<p.pid<<",\"birth\":"<<p.birth<<",\"owner\":"
        <<std::quoted(p.owner)<<",\"session\":"<<p.session;
}
const char* label(ReadStatus s) {
    switch(s) {
    case ReadStatus::Fresh:return "fresh"; case ReadStatus::Stale:return "stale";
    case ReadStatus::NotReady:return "not-ready"; case ReadStatus::Missing:return "missing";
    case ReadStatus::Invalid:return "invalid"; case ReadStatus::WrongOwner:return "wrong-owner";
    }
    return "invalid";
}
}
int main(int argc,char** argv) {
    try {
        Snapshot s; ReadStatus status=ReadStatus::Invalid;
        const auto now=monotonicMillis();
        if (argc==3 && std::string(argv[1])=="--identity") {
            ProcessIdentity p;
            if (!processIdentity(decimal(argv[2]),p)) return 2;
            std::cout<<"{"; identity(p); std::cout<<"}\n"; return 0;
        } else if (argc==2 && std::string(argv[1])=="--published") {
            const auto self=currentProcessIdentity();
            status=readPublishedSnapshot(self.owner,self.session,now,s);
        } else if (argc==5 && std::string(argv[1])=="--pid" && std::string(argv[3])=="--birth") {
            ProcessIdentity p;
            if (!processIdentity(decimal(argv[2]),p) || p.birth!=decimal(argv[4])) return 2;
            status=readSnapshot(privateSnapshotDirectory(),p,now,s);
        } else {
            std::cerr<<"usage: topologysnapshotprobe --identity PID | --pid PID --birth BIRTH | --published\n";
            return 2;
        }
        std::cout<<"{\"status\":"<<std::quoted(label(status))<<","; identity(s.identity);
        std::cout<<",\"ownerGeneration\":"<<s.ownerGeneration<<",\"run\":"<<s.run
            <<",\"sequence\":"<<s.sequence<<",\"updatedMillis\":"<<s.updatedMillis
            <<",\"nowMillis\":"<<now<<",\"ready\":"<<(s.ready?"true":"false")
            <<",\"localName\":"<<std::quoted(s.localName)<<",\"localRole\":"<<unsigned(s.localRole)
            <<",\"epoch\":"<<s.topology.epoch<<",\"revision\":"<<s.topology.revision
            <<",\"layout\":"<<unsigned(s.topology.layout)<<",\"nodes\":[";
        bool comma=false;
        for (const auto& n:s.topology.nodes) {
            if (comma) std::cout<<","; comma=true;
            std::cout<<"{\"name\":"<<std::quoted(n.name)<<",\"role\":"<<unsigned(n.role)
                <<",\"x\":"<<n.x<<",\"y\":"<<n.y
                <<",\"reportedOnline\":"<<(n.online?"true":"false")
                <<",\"liveOnline\":"<<(status==ReadStatus::Fresh&&n.online?"true":"false")<<"}";
        }
        std::cout<<"]}\n";
        return status==ReadStatus::Fresh?0:1;
    } catch (const std::exception& e) {
        std::cerr<<e.what()<<"\n"; return 2;
    }
}
