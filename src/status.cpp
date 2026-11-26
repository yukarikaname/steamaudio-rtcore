#include "rtcore/rtcore.h"

namespace rtcore {

const char* toString(Status status) {
    switch (status) {
    case Status::ok:
        return "ok";
    case Status::invalidArgument:
        return "invalid_argument";
    case Status::unsupported:
        return "unsupported";
    case Status::outOfMemory:
        return "out_of_memory";
    case Status::deviceError:
        return "device_error";
    case Status::notBuilt:
        return "not_built";
    }
    return "unknown";
}

} // namespace rtcore
