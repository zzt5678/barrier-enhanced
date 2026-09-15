#pragma once
#include "server/ClientProxy1_12.h"

// Protocol 1.13 = 1.12 input/clipboard compatibility plus read-only topology.
class ClientProxy1_13 : public ClientProxy1_12 {
public:
    using ClientProxy1_12::ClientProxy1_12;
    void sendDeviceTopology(const weave::topology::Topology&) override;
};
