// wowee_pack: makes the drop-in executable - a copy of wowee.exe with this
// client's own files appended (see include/core/dropin.hpp for the reader).
//
//   wowee_pack <in.exe> <out.exe> <root> <dir-or-file>...
//
// Every file under each <root>/<dir-or-file> is appended under its path
// relative to <root>, followed by an index and a 24-byte trailer:
// "WOWEEPK1", the index offset (u64) and the index size (u64). The index is
// u32 count, then per file u16 path length, the path ('/' separated), u64
// offset in the executable and u64 size. All little-endian.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
template <typename T>
void put(std::vector<char>& v, T x) {
    const char* p = reinterpret_cast<const char*>(&x);
    v.insert(v.end(), p, p + sizeof(T));
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: wowee_pack <in.exe> <out.exe> <root> <dir-or-file>...\n";
        return 2;
    }
    const fs::path in = argv[1], out = argv[2], root = argv[3];
    std::error_code ec;
    fs::copy_file(in, out, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "cannot copy " << in << ": " << ec.message() << "\n";
        return 1;
    }
    std::vector<fs::path> files;
    for (int i = 4; i < argc; ++i) {
        const fs::path p = root / argv[i];
        if (fs::is_regular_file(p, ec)) {
            files.push_back(p);
        } else if (fs::is_directory(p, ec)) {
            for (auto it = fs::recursive_directory_iterator(p, ec); it != fs::recursive_directory_iterator();
                 it.increment(ec)) {
                if (it->is_regular_file(ec)) files.push_back(it->path());
            }
        } else {
            std::cerr << "skipped (missing): " << p << "\n";
        }
    }
    std::fstream o(out, std::ios::binary | std::ios::in | std::ios::out | std::ios::ate);
    if (!o) {
        std::cerr << "cannot open " << out << "\n";
        return 1;
    }
    uint64_t at = static_cast<uint64_t>(o.tellp());
    std::vector<char> index;
    put<uint32_t>(index, static_cast<uint32_t>(files.size()));
    uint64_t total = 0;
    std::vector<char> buf;
    for (const auto& f : files) {
        std::ifstream src(f, std::ios::binary | std::ios::ate);
        const uint64_t size = static_cast<uint64_t>(src.tellg());
        src.seekg(0);
        buf.resize(size);
        src.read(buf.data(), static_cast<std::streamsize>(size));
        o.write(buf.data(), static_cast<std::streamsize>(size));
        std::string rel = fs::relative(f, root, ec).generic_string();
        put<uint16_t>(index, static_cast<uint16_t>(rel.size()));
        index.insert(index.end(), rel.begin(), rel.end());
        put<uint64_t>(index, at);
        put<uint64_t>(index, size);
        at += size;
        total += size;
    }
    const uint64_t indexAt = at;
    o.write(index.data(), static_cast<std::streamsize>(index.size()));
    std::vector<char> trailer = {'W', 'O', 'W', 'E', 'E', 'P', 'K', '1'};
    put<uint64_t>(trailer, indexAt);
    put<uint64_t>(trailer, static_cast<uint64_t>(index.size()));
    o.write(trailer.data(), static_cast<std::streamsize>(trailer.size()));
    if (!o.good()) {
        std::cerr << "write failed\n";
        return 1;
    }
    std::cout << "wowee_pack: " << files.size() << " files, " << total / 1024 << " KB appended to " << out.string()
              << "\n";
    return 0;
}
