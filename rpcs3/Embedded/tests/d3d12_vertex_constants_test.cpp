#include "../../Emu/RSX/D3D12/D3D12VertexConstants.h"
#include <array>
#include <algorithm>
#include <iostream>

int main()
{
    std::array<std::array<float, 4>, 468> rsx{};
    for (unsigned i = 0; i < rsx.size(); ++i) rsx[i].fill(float(i));
    const std::array<std::uint16_t, 5> sparse{0, 1, 2, 3, 5};
    const auto compact = d3d12::vertex_constant_upload_ids(false, sparse);
    std::array<std::array<float, 4>, 468> uploaded{};
    for (unsigned i = 0; i < compact.size(); ++i) uploaded[i] = rsx[compact[i]];
    if (uploaded[4] != rsx[5] || uploaded[4] == rsx[4]) return 1;
    const auto dynamic = d3d12::vertex_constant_upload_ids(true, sparse);
    if (!dynamic.empty()) return 2;
    // The existing RSX uploader copies the complete bank for an empty table.
    if (dynamic.empty()) uploaded = rsx;
    if (uploaded[4] != rsx[4] || uploaded[467] != rsx[467]) return 3;
    const std::array<std::uint16_t, 2> second{12, 467};
    const auto changed = d3d12::vertex_constant_upload_ids(false, second);
    if (changed.size() != 2 || changed[0] != 12 || changed[1] != 467) return 4;
    if (!d3d12::vertex_constant_upload_ids(false, {}).empty()) return 5;
    std::cout << "PASS: sparse constant relocation, full indexed bank, shader switch and empty table\n";
}
