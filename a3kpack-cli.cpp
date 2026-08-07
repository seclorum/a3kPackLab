//
// a3kpack-cli.cpp
//
// Command-line pack/unpack for Yamaha A3000 ".a3k" archives, built on the
// A3kPack class (a3kPack.cpp).  Mirrors a3kpacker.py.
//
// Usage:
//   a3kpack-cli unpack archive.a3k [outdir]
//   a3kpack-cli pack   indir [out.a3k]
//

#include "a3kPack.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void printUsage() {
    std::cout
        << "a3kpack-cli - pack/unpack Yamaha A3000 .a3k archives\n"
        << "\n"
        << "Usage:\n"
        << "  a3kpack-cli unpack archive.a3k [outdir]\n"
        << "  a3kpack-cli pack   indir [out.a3k]\n";
}

std::string readTextFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

std::streamsize fileSize(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return 0;
    return in.tellg();
}

// Read volume + file count from a manifest.txt produced by unpack().
void manifestInfo(const std::string& indir, std::string& volume, size_t& files) {
    volume.clear();
    files = 0;
    std::ifstream mf((indir + "/manifest.txt").c_str());
    if (!mf) return;
    std::string line;
    while (std::getline(mf, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("volume=", 0) == 0) {
            volume = line.substr(7);
        } else if (line.find('\t') != std::string::npos) {
            ++files;
        }
    }
}

int cmdUnpack(int argc, char** argv) {
    std::string fn = argv[2];
    std::string outdir = argc > 3 ? argv[3] : fn + ".unpacked";

    A3kPack tool;
    A3kPack::UnpackInfo info;
    if (!tool.unpack(fn, outdir, &info)) {
        std::cerr << "unpack failed: " << tool.lastError() << "\n";
        return 1;
    }

    std::cout << "Archive : " << fn << "\n";
    std::cout << "Volume  : '" << info.volume << "'\n";
    std::cout << "Banner  :\n" << readTextFile(outdir + "/A3kFileInfo.txt") << "\n";
    std::cout << "\nUnpacked " << info.files.size() << " files to " << outdir << "/\n";
    for (const std::string& f : info.files) {
        std::istringstream ss(f);
        std::string typ, fname, sz;
        std::getline(ss, typ, '\t');
        std::getline(ss, fname, '\t');
        std::getline(ss, sz);
        std::cout << "  " << typ << "  '" << fname << "'  " << sz << " bytes\n";
    }
    return 0;
}

int cmdPack(int argc, char** argv) {
    std::string indir = argv[2];
    std::string outfn = argc > 3 ? argv[3] : indir + ".a3k";

    A3kPack tool;
    if (!tool.pack(indir, outfn)) {
        std::cerr << "pack failed: " << tool.lastError() << "\n";
        return 1;
    }

    std::string volume;
    size_t files = 0;
    manifestInfo(indir, volume, files);

    std::cout << "Packed  : " << outfn << "\n";
    std::cout << "Volume  : '" << volume << "'\n";
    std::cout << "Files   : " << files << "\n";
    std::cout << "Size    : " << fileSize(outfn) << " bytes\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 1;
    }
    std::string cmd = argv[1];
    if (cmd == "unpack" && argc >= 3) return cmdUnpack(argc, argv);
    if (cmd == "pack" && argc >= 3) return cmdPack(argc, argv);
    printUsage();
    return 1;
}
