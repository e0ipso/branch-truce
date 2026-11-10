// UTF-8 checks for the engine's text and the byte ranges that address it.
#include "Utf8.h"

#include <cstdint>

namespace branch_truce {
namespace {
bool boundary(std::string_view text, std::size_t position)
{
    return position <= text.size()
        && (position == text.size() || (static_cast<unsigned char>(text[position]) & 0xc0U) != 0x80U);
}
}

bool validUtf8(std::string_view text) noexcept
{
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first <= 0x7fU) continue;
        std::size_t count = 0;
        std::uint32_t value = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2U && first <= 0xdfU) {
            count = 1;
            value = first & 0x1fU;
            minimum = 0x80;
        } else if (first >= 0xe0U && first <= 0xefU) {
            count = 2;
            value = first & 0x0fU;
            minimum = 0x800;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            count = 3;
            value = first & 0x07U;
            minimum = 0x10000;
        } else
            return false;
        if (count > text.size() - i) return false;
        for (std::size_t n = 0; n < count; ++n) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0U) != 0x80U) return false;
            value = (value << 6U) | (next & 0x3fU);
        }
        if (value < minimum || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU)) return false;
    }
    return true;
}

bool validByteRange(std::string_view text, ByteRange range) noexcept
{
    return range.begin <= range.end && boundary(text, range.begin) && boundary(text, range.end);
}

}
