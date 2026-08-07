//
// a3kPack.h
//
// Pack / unpack Yamaha A3000 ".a3k" archives (the "A3kDiskyPC" container).
//
// The ".a3k" files are produced by the Windows program "A3kDisky" (a Yamaha
// A3000 sampler librarian).  They are NOT zip/lzh/arc: they use a custom
// container that stores raw SFS file images.  This class mirrors the Python
// tool a3kpacker.py included in this folder.
//
// Container layout
// ----------------
//   [0:4]      version (little-endian, always 1)
//   [4:8]      dsz  = start offset of the file-info section (little-endian)
//   [8:12]     file count (little-endian) == number of file-info entries
//   [12:22]    magic "A3kDiskyPC"
//   [22:70]    padding (zeros)
//   [70:86]    marker "XXXXXXXXXXXXXXXX"
//   [86:1110]  padding (zeros)
//   [1110:dsz] the volume banner/description text (entry 0 of the file info)
//   [dsz:EOF]  file-info section, exactly file_count * 271 bytes
//
// File-info section (271 bytes per entry)
// ---------------------------------------
//   [0:256]    path  "VolumeName \TYPE\FileName" (NUL terminated, zero padded)
//   [256:258]  marker (01 00 for files, 00 00 for the banner entry)
//   [258:262]  offset of the entry in the file (little-endian)
//   [262:266]  size   of the entry (little-endian)
//   [266:270]  01 00 00 00
//   [270]      00
//
// Entry 0 is always "/A3kFileInfo.txt" and points at the banner text.  Every
// following entry is a raw SFS file image beginning with the magic
// "FSFSDEV3SPLX" followed by a 4-byte type (PROG / SBAC / SBNK / SMPL / SEQU).
//

#pragma once

#include <cstdint>
#include <string>
#include <vector>

class A3kPack {
public:
    // One entry in the file-info index.
    struct Entry {
        std::string path;   // "VolumeName \TYPE\FileName"
        uint32_t offset = 0;
        uint32_t size = 0;
    };

    // Summary returned by unpack().
    struct UnpackInfo {
        std::string volume;
        std::vector<std::string> files; // "TYPE\tfname\tsize"
    };

    // Unpack an .a3k archive into outDir, writing A3kFileInfo.txt,
    // manifest.txt and one NNN_TYPE_fname.bin per SFS entry.
    // Returns false on failure (see lastError()).
    bool unpack(const std::string& archivePath, const std::string& outDir,
                UnpackInfo* info = nullptr);

    // Pack a directory (as produced by unpack, or a directory of full FSFS
    // .bin entries) into an .a3k archive.  Returns false on failure.
    bool pack(const std::string& inDir, const std::string& outArchive);

    // Last error message (empty when the last call succeeded).
    const std::string& lastError() const { return lastError_; }

private:
    static constexpr uint32_t kEntrySize = 271;
    static constexpr uint32_t kHeaderSize = 1110;
    static constexpr char kMagic[] = "FSFSDEV3SPLX";

    bool readFile(const std::string& path, std::vector<uint8_t>& out);
    bool writeFile(const std::string& path, const std::vector<uint8_t>& data);
    bool parseArchive(const std::vector<uint8_t>& data, uint32_t& version,
                      uint32_t& dsz, uint32_t& count, std::vector<Entry>& entries);
    std::string entryType(const std::vector<uint8_t>& entry) const;
    std::string sanitize(const std::string& name) const;
    std::vector<uint8_t> makeFinfoEntry(const std::string& path, uint32_t off,
                                        uint32_t size, bool isBanner) const;

    std::string lastError_;
};
