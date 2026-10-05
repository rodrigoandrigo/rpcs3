#include "stdafx.h"
#include <iostream>
struct SelfAdditionalInfo;
fs::file decrypt_self(const fs::file&, const u8*, SelfAdditionalInfo*);

// Offline SELF inspection only. Does not initialize or execute a guest.
int main(int argc, char** argv) try
{
    if (argc != 3) return 2;
    fs::file input(argv[1]);
    if (!input) return 3;
    auto elf = decrypt_self(input, nullptr, nullptr);
    if (!elf) return 4;
    fs::file output(argv[2], fs::rewrite);
    if (!output) return 5;
    std::vector<u8> bytes(elf.size());
    elf.seek(0);
    if (elf.read(bytes.data(), bytes.size()) != bytes.size()) return 6;
    if (output.write(bytes.data(), bytes.size()) != bytes.size()) return 7;
    std::cout << "Decrypted ELF bytes: " << bytes.size() << '\n';
    return 0;
}
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
