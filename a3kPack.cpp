//
// a3kPack.cpp
//
// Implementation of A3kPack - pack/unpack Yamaha A3000 ".a3k" archives.
//

#include "a3kPack.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

// Little-endian helpers (host is assumed little-endian for the file-info
// section, which is what A3kDisky wrote on x86).
inline uint32_t rdLE32(const std::vector<uint8_t>& b, size_t off) {
    return static_cast<uint32_t>(b[off])
         | (static_cast<uint32_t>(b[off + 1]) << 8)
         | (static_cast<uint32_t>(b[off + 2]) << 16)
         | (static_cast<uint32_t>(b[off + 3]) << 24);
}

inline void wrLE32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    b[off]     = static_cast<uint8_t>(v & 0xff);
    b[off + 1] = static_cast<uint8_t>((v >> 8) & 0xff);
    b[off + 2] = static_cast<uint8_t>((v >> 16) & 0xff);
    b[off + 3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

} // namespace

// ---------------------------------------------------------------------------
// file helpers
// ---------------------------------------------------------------------------

bool A3kPack::readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        lastError_ = "cannot open: " + path;
        return false;
    }
    in.seekg(0, std::ios::end);
    std::streamoff len = in.tellg();
    in.seekg(0, std::ios::beg);
    if (len < 0) {
        lastError_ = "cannot size: " + path;
        return false;
    }
    out.resize(static_cast<size_t>(len));
    if (len > 0) in.read(reinterpret_cast<char*>(out.data()), len);
    return true;
}

bool A3kPack::writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        lastError_ = "cannot write: " + path;
        return false;
    }
    if (!data.empty())
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    return true;
}

// ---------------------------------------------------------------------------
// parsing
// ---------------------------------------------------------------------------

bool A3kPack::parseArchive(const std::vector<uint8_t>& data, uint32_t& version,
                           uint32_t& dsz, uint32_t& count,
                           std::vector<Entry>& entries) {
    if (data.size() < kHeaderSize) {
        lastError_ = "file too small to be an A3kDiskyPC archive";
        return false;
    }
    if (data.size() < 22 || std::string(data.begin() + 12, data.begin() + 22) != "A3kDiskyPC") {
        lastError_ = "not an A3kDiskyPC archive";
        return false;
    }
    version = rdLE32(data, 0);
    dsz = rdLE32(data, 4);
    count = rdLE32(data, 8);

    const size_t finfoSize = static_cast<size_t>(count) * kEntrySize;
    if (finfoSize > data.size()) {
        lastError_ = "file-info section exceeds file size";
        return false;
    }
    const size_t finfoStart = data.size() - finfoSize;
    entries.clear();
    entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t base = finfoStart + static_cast<size_t>(i) * kEntrySize;
        Entry e;
        // path is NUL-terminated within the first 256 bytes
        const char* p = reinterpret_cast<const char*>(&data[base]);
        size_t n = 0;
        while (n < 256 && p[n] != '\0') ++n;
        e.path.assign(p, n);
        e.offset = rdLE32(data, base + 258);
        e.size = rdLE32(data, base + 262);
        entries.push_back(std::move(e));
    }
    return true;
}

std::string A3kPack::entryType(const std::vector<uint8_t>& entry) const {
    if (entry.size() < 16) return "";
    return std::string(entry.begin() + 12, entry.begin() + 16);
}

std::string A3kPack::sanitize(const std::string& name) const {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'
            || c == '-' || c == ' ')
            out.push_back(c);
        else
            out.push_back('_');
    }
    // trim leading/trailing underscores and spaces
    size_t b = out.find_first_not_of("_ ");
    size_t e = out.find_last_not_of("_ ");
    if (b == std::string::npos) return "file";
    return out.substr(b, e - b + 1);
}

// ---------------------------------------------------------------------------
// unpack
// ---------------------------------------------------------------------------

bool A3kPack::unpack(const std::string& archivePath, const std::string& outDir,
                     UnpackInfo* info) {
    lastError_.clear();
    std::vector<uint8_t> data;
    if (!readFile(archivePath, data)) return false;

    uint32_t version = 0, dsz = 0, count = 0;
    std::vector<Entry> entries;
    if (!parseArchive(data, version, dsz, count, entries)) return false;

    fs::create_directories(outDir);

    // banner (entry 0)
    const Entry& bannerEntry = entries[0];
    std::vector<uint8_t> banner(data.begin() + bannerEntry.offset,
                                data.begin() + bannerEntry.offset + bannerEntry.size);
    if (!writeFile((fs::path(outDir) / "A3kFileInfo.txt").string(), banner)) return false;

    // volume name from the first file entry's path
    std::string volume;
    if (count > 1) {
        const std::string& p = entries[1].path;
        size_t slash = p.find('\\');
        volume = (slash == std::string::npos) ? p : p.substr(0, slash);
    }

    // index width for zero-padded file names
    int width = 1;
    for (uint32_t c = count; c >= 10; c /= 10) ++width;

    std::vector<std::string> manifestLines;
    manifestLines.push_back("# a3kpacker manifest");
    manifestLines.push_back("volume=" + volume);

    if (info) info->files.clear();

    for (uint32_t i = 1; i < count; ++i) {
        const Entry& e = entries[i];
        std::vector<uint8_t> entry(data.begin() + e.offset,
                                   data.begin() + e.offset + e.size);
        std::string typ = entryType(entry);

        std::string fname = e.path;
        size_t slash = fname.rfind('\\');
        if (slash != std::string::npos) fname = fname.substr(slash + 1);
        // trim trailing spaces
        size_t end = fname.find_last_not_of(' ');
        if (end != std::string::npos) fname = fname.substr(0, end + 1);

        std::string fnameSafe = sanitize(fname);
        std::ostringstream oss;
        oss.width(width);
        oss.fill('0');
        oss << i;
        std::string outname = oss.str() + "_" + typ + "_" + fnameSafe + ".bin";

        if (!writeFile((fs::path(outDir) / outname).string(), entry)) return false;

        manifestLines.push_back(e.path + "\t" + outname);
        if (info) {
            std::ostringstream s;
            s << typ << "\t" << fname << "\t" << e.size;
            info->files.push_back(s.str());
        }
    }

    std::ofstream mf((fs::path(outDir) / "manifest.txt").string());
    if (!mf) {
        lastError_ = "cannot write manifest.txt";
        return false;
    }
    for (const std::string& line : manifestLines) mf << line << "\n";

    if (info) info->volume = volume;
    return true;
}

// ---------------------------------------------------------------------------
// pack
// ---------------------------------------------------------------------------

std::vector<uint8_t> A3kPack::makeFinfoEntry(const std::string& path, uint32_t off,
                                             uint32_t size, bool isBanner) const {
    std::vector<uint8_t> e(kEntrySize, 0);
    // path field: 256 bytes, NUL padded
    size_t n = std::min<size_t>(path.size(), 256);
    std::copy(path.begin(), path.begin() + n, e.begin());
    // marker
    e[256] = isBanner ? 0x00 : 0x01;
    e[257] = 0x00;
    wrLE32(e, 258, off);
    wrLE32(e, 262, size);
    e[266] = 0x01;
    e[267] = 0x00;
    e[268] = 0x00;
    e[269] = 0x00;
    e[270] = 0x00;
    return e;
}

bool A3kPack::pack(const std::string& inDir, const std::string& outArchive) {
    lastError_.clear();
    const fs::path dir(inDir);

    // ---- read manifest (or fall back to scanning .bin files) ----
    std::string volume;
    std::vector<std::pair<std::string, std::string>> entries; // (path, filename)
    fs::path manifestPath = dir / "manifest.txt";
    if (fs::exists(manifestPath)) {
        std::ifstream mf(manifestPath.string());
        if (!mf) {
            lastError_ = "cannot read manifest.txt";
            return false;
        }
        std::string line;
        while (std::getline(mf, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (line.rfind("volume=", 0) == 0) {
                volume = line.substr(7);
                continue;
            }
            size_t tab = line.find('\t');
            if (tab != std::string::npos)
                entries.emplace_back(line.substr(0, tab), line.substr(tab + 1));
        }
    } else {
        // scan directory for full FSFS .bin entries
        volume = "New Volume";
        fs::path bannerPath = dir / "A3kFileInfo.txt";
        if (fs::exists(bannerPath)) {
            std::vector<uint8_t> banner;
            if (readFile(bannerPath.string(), banner)) {
                std::string text(banner.begin(), banner.end());
                size_t pos = text.find("Volume Name");
                if (pos != std::string::npos) {
                    pos = text.find(':', pos);
                    if (pos != std::string::npos) {
                        size_t b = text.find_first_not_of(" \t", pos + 1);
                        size_t e = text.find_first_of("\r\n", b);
                        volume = text.substr(b, e - b);
                    }
                }
            }
        }
        for (const auto& de : fs::directory_iterator(dir)) {
            if (!de.is_regular_file()) continue;
            std::string name = de.path().filename().string();
            if (name.size() < 4 ||
                name.compare(name.size() - 4, 4, ".bin") != 0)
                continue;
            std::vector<uint8_t> entry;
            if (!readFile(de.path().string(), entry)) continue;
            if (entry.size() < 16 ||
                std::string(entry.begin(), entry.begin() + 12) != kMagic)
                continue;
            std::string typ = entryType(entry);
            std::string fname = name.substr(0, name.size() - 4);
            // strip leading index and type prefixes
            size_t u = fname.find('_');
            if (u != std::string::npos) {
                bool allDigits = !fname.substr(0, u).empty()
                    && std::all_of(fname.begin(), fname.begin() + u,
                                   [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
                if (allDigits) fname = fname.substr(u + 1);
            }
            if (fname.rfind(typ + "_", 0) == 0) fname = fname.substr(typ.size() + 1);
            entries.emplace_back(volume + " \\" + typ + "\\" + fname, name);
        }
        std::sort(entries.begin(), entries.end());
    }

    // ---- banner ----
    std::vector<uint8_t> banner;
    fs::path bannerPath = dir / "A3kFileInfo.txt";
    if (fs::exists(bannerPath)) {
        if (!readFile(bannerPath.string(), banner)) return false;
    } else {
        std::string text = "\r\n          A3kDisky Volume ArKive\r\n"
                           "-------------------------------------------\r\n"
                           "       Volume Name : " + volume + "\r\n";
        banner.assign(text.begin(), text.end());
    }

    // ---- load each entry's full bytes ----
    std::vector<std::vector<uint8_t>> blobs;
    blobs.reserve(entries.size());
    for (const auto& pr : entries) {
        std::vector<uint8_t> blob;
        if (!readFile((dir / pr.second).string(), blob)) return false;
        blobs.push_back(std::move(blob));
    }

    const uint32_t count = 1 + static_cast<uint32_t>(entries.size());
    uint32_t off = kHeaderSize + static_cast<uint32_t>(banner.size());
    for (const auto& b : blobs) off += static_cast<uint32_t>(b.size());
    const uint32_t dsz = off;

    // ---- header ----
    std::vector<uint8_t> out(kHeaderSize, 0);
    wrLE32(out, 0, 1);
    wrLE32(out, 4, dsz);
    wrLE32(out, 8, count);
    const std::string magic1 = "A3kDiskyPC";
    std::copy(magic1.begin(), magic1.end(), out.begin() + 12);
    const std::string magic2 = "XXXXXXXXXXXXXXXX";
    std::copy(magic2.begin(), magic2.end(), out.begin() + 70);

    out.insert(out.end(), banner.begin(), banner.end());
    for (const auto& b : blobs) out.insert(out.end(), b.begin(), b.end());

    // ---- file-info section ----
    std::vector<uint8_t> finfo = makeFinfoEntry("/A3kFileInfo.txt", kHeaderSize,
                                                static_cast<uint32_t>(banner.size()),
                                                /*isBanner=*/true);
    off = kHeaderSize + static_cast<uint32_t>(banner.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        std::vector<uint8_t> fe = makeFinfoEntry(entries[i].first, off,
                                                 static_cast<uint32_t>(blobs[i].size()),
                                                 /*isBanner=*/false);
        finfo.insert(finfo.end(), fe.begin(), fe.end());
        off += static_cast<uint32_t>(blobs[i].size());
    }
    out.insert(out.end(), finfo.begin(), finfo.end());

    return writeFile(outArchive, out);
}
