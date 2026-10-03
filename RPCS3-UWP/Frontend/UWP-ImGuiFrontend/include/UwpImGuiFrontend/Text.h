#pragma once

#include <string>
#include <string_view>

struct ImFont;

namespace UwpImGuiFrontend
{
// Decodes character references and repairs Windows-1252 bytes to valid UTF-8.
[[nodiscard]] std::string NormalizeDisplayText(std::string_view value);

// Normalizes supported metadata dates to YYYY-MM-DD.
[[nodiscard]] std::string NormalizeMetadataReleaseDate(std::string_view value);

// Formats valid dates as DD/MM/YYYY and leaves other values unchanged.
[[nodiscard]] std::string FormatMetadataReleaseDateUk(std::string_view value);

// Selects a Unicode-capable font from the host's ImGui atlas.
[[nodiscard]] ImFont* ResolveFrontendTextFont(ImFont* preferred = nullptr);
}
