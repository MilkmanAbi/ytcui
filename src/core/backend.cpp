#include "core/backend.h"

#include "core/backend_switch.h"
#include "core/log.h"

namespace ytcui {

std::unique_ptr<Backend> make_backend(config::Backend which) {
    // Always the switching wrapper, even when only one extractor is wanted:
    // it is what makes ^B and automatic failover possible at all, and it costs
    // nothing until a second backend is actually asked for.
    return std::make_unique<SwitchingBackend>(which, /*allow_failover=*/true);
}

std::unique_ptr<Backend> make_backend(config::Backend which, bool allow_failover) {
    return std::make_unique<SwitchingBackend>(which, allow_failover);
}

}  // namespace ytcui
