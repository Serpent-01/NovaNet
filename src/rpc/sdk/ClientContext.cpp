#include "novanet/rpc/sdk/ClientContext.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace novanet::rpc::sdk {

using novanet::base::addTime;
using novanet::base::timeDifference;
using novanet::base::Timestamp;
namespace meta = novanet::rpc::meta;

ClientContext::ClientContext() = default;

void ClientContext::setTimeoutSeconds(double seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
}

}  // namespace novanet::rpc::sdk