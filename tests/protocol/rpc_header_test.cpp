#include "novanet/rpc/protocol/RpcHeader.h"
#include "novanet/rpc/protocol/FrameType.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace novanet::rpc{
namespace {
std::uint16_t raw(FrameType type){
    return static_cast<std::uint16_t>(type);
}
}    
}