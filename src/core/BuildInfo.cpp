// Build information supplies the application version used by the desktop and command line.
#include "BuildInfo.h"

#ifndef BRANCH_TRUCE_VERSION_STRING
#error "BRANCH_TRUCE_VERSION_STRING must name the resolved application version"
#endif

namespace branch_truce {
std::string_view version() noexcept { return BRANCH_TRUCE_VERSION_STRING; }
}
