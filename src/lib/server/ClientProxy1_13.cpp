#include "ClientProxy1_13.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/protocol_types.h"
#include "io/IStream.h"

void ClientProxy1_13::sendDeviceTopology(const weave::topology::Topology& t) {
    // Periodic full-state updates may be skipped for a congested peer. Its GUI
    // expires the previous state; topology must never grow the input backlog.
    if (getStream()->getBufferedOutputSize() > weave::topology::MaxBytes) return;
    std::string bytes;
    if (weave::topology::encodeTopology(t,bytes) && bytes.size()+8<=weave::topology::MaxBytes)
        ProtocolUtil::writef(getStream(),kMsgDTopology,&bytes);
}
