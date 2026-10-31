// Build information supplies the application version used by the desktop and command line.
#pragma once
#include <string_view>

namespace branch_truce {
[[nodiscard]] std::string_view version() noexcept;
}
