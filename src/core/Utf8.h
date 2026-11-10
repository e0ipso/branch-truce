// UTF-8 checks for the engine's text and the byte ranges that address it.
#pragma once

#include "ByteRanges.h"
#include <string_view>

namespace branch_truce {

// Whether the text is well-formed UTF-8: no overlong forms, surrogates or
// code points past U+10FFFF.
[[nodiscard]] bool validUtf8(std::string_view text) noexcept;
// Whether the range is ordered and both its ends lie on character boundaries
// of the text.
[[nodiscard]] bool validByteRange(std::string_view text, ByteRange range) noexcept;

}
