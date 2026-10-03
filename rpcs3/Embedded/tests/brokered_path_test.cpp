#include "../brokered_path.h"
#include <iostream>
#include <stdexcept>
using namespace rpcs3::embedded;
int main() try
{
    const std::string root = "/vfsv0_abcdefghijklmnopqrstuv_games";
    std::vector<std::string> parts;
    auto check = [](bool value) { if (!value) throw std::runtime_error("Brokered path test failed"); };
    check(brokered_relative_parts(root + "/PS3_GAME/./USRDIR/EBOOT.BIN", root, parts));
    check(parts == std::vector<std::string>({"PS3_GAME", "USRDIR", "EBOOT.BIN"}));
    check(brokered_relative_parts(root + "/PS3_GAME/../PS3_DISC.SFB", root, parts));
    check(parts == std::vector<std::string>({"PS3_DISC.SFB"}));
    check(brokered_relative_parts(root + "/folder\\file", root, parts));
    for (const auto& bad : {"/../secret", "/a/../../secret", "/C:/secret", "/file:stream", "/folder. /file", "/folder./file"})
        check(!brokered_relative_parts(root + bad, root, parts));
    check(!brokered_relative_parts(root + "evil/file", root, parts));
    check(!brokered_relative_parts("C:/outside", root, parts));
    check(!brokered_relative_parts(root + std::string("/file\0suffix", 12), root, parts));
    std::cout << "PASS: broker root confinement, traversal, alternate streams, separator normalization\n";
    return 0;
}
catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
