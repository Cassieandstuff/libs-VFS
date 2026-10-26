#include <vfs/Bsa.h>

#include <lz4frame.h>

#include <cstring>
#include <fstream>
#include <memory>

namespace vfs::bsa {

    namespace {

        constexpr std::uint32_t kMagic   = 0x00415342;  ///< "BSA\0"
        constexpr std::uint32_t kVersion = 105;

        constexpr std::uint32_t kHasDirNames  = 0x001;
        constexpr std::uint32_t kHasFileNames = 0x002;
        constexpr std::uint32_t kCompressed   = 0x004;
        constexpr std::uint32_t kEmbedNames   = 0x100;

        constexpr std::uint32_t kSizeToggle = 0x40000000;
        constexpr std::uint32_t kSizeMask   = 0x3FFFFFFF;

        constexpr std::size_t kHeaderSize      = 36;
        constexpr std::size_t kFolderRecord105 = 24;
        constexpr std::size_t kFileRecord      = 16;

        /// @brief `p` as UTF-8 text (path::string() throws on Windows for a name outside the ANSI code page).
        std::string U8Path(const std::filesystem::path& p) {
            const std::u8string s = p.u8string();
            return std::string(s.begin(), s.end());
        }

        bool Fail(std::string* err, std::string msg) {
            if (err) *err = std::move(msg);
            return false;
        }

        class Cursor {
        public:
            Cursor(const std::uint8_t* data, std::size_t size) : m_data(data), m_size(size) {}

            template <class T>
            bool Read(T& v) {
                if (m_pos + sizeof(T) > m_size) return false;
                std::memcpy(&v, m_data + m_pos, sizeof(T));
                m_pos += sizeof(T);
                return true;
            }
            bool Skip(std::size_t n) {
                if (m_pos + n > m_size) return false;
                m_pos += n;
                return true;
            }
            bool Seek(std::size_t pos) {
                if (pos > m_size) return false;
                m_pos = pos;
                return true;
            }
            [[nodiscard]] std::size_t          Pos() const noexcept { return m_pos; }
            [[nodiscard]] std::size_t          Remaining() const noexcept { return m_size - m_pos; }
            [[nodiscard]] const std::uint8_t*  Here() const noexcept { return m_data + m_pos; }

        private:
            const std::uint8_t* m_data;
            std::size_t         m_size;
            std::size_t         m_pos = 0;
        };

        struct FolderRecord {
            std::uint32_t count = 0;
            std::uint64_t offset = 0;
        };

    }  // namespace

    std::optional<Archive> Archive::Open(const std::filesystem::path& path, std::string* err) {
        std::ifstream f(path, std::ios::binary);
        if (!f) { Fail(err, "cannot open " + U8Path(path)); return std::nullopt; }

        std::uint8_t hdr[kHeaderSize];
        if (!f.read(reinterpret_cast<char*>(hdr), kHeaderSize)) { Fail(err, "truncated header"); return std::nullopt; }
        Cursor h(hdr, kHeaderSize);
        std::uint32_t magic, version, dirOffset, flags, folderCount, fileCount, folderNamesLen, fileNamesLen;
        h.Read(magic); h.Read(version); h.Read(dirOffset); h.Read(flags);
        h.Read(folderCount); h.Read(fileCount); h.Read(folderNamesLen); h.Read(fileNamesLen);

        if (magic != kMagic) { Fail(err, "not a BSA (bad magic)"); return std::nullopt; }
        if (version != kVersion) {
            Fail(err, "BSA version " + std::to_string(version) + " unsupported (SSE/AE archives are v105; v104 is Skyrim LE)");
            return std::nullopt;
        }
        if (!(flags & kHasDirNames) || !(flags & kHasFileNames)) {
            Fail(err, "BSA lacks directory/file name tables — paths are unrecoverable");
            return std::nullopt;
        }

        const std::size_t recordsStart = dirOffset + static_cast<std::size_t>(folderCount) * kFolderRecord105;
        const std::size_t namesStart   = recordsStart + folderNamesLen + folderCount +
                                         static_cast<std::size_t>(fileCount) * kFileRecord;
        const std::size_t dirEnd       = namesStart + fileNamesLen;
        std::vector<std::uint8_t> dir(dirEnd);
        f.seekg(0);
        if (!f.read(reinterpret_cast<char*>(dir.data()), static_cast<std::streamsize>(dirEnd))) {
            Fail(err, "truncated directory");
            return std::nullopt;
        }
        Cursor c(dir.data(), dir.size());

        std::vector<FolderRecord> folders(folderCount);
        c.Seek(dirOffset);
        for (auto& fr : folders) {
            std::uint64_t hash;
            std::uint32_t pad;
            if (!c.Read(hash) || !c.Read(fr.count) || !c.Read(pad) || !c.Read(fr.offset)) {
                Fail(err, "truncated folder records");
                return std::nullopt;
            }
        }

        Archive a;
        a.m_path = path;
        a.m_version = version;
        a.m_flags = flags;
        a.m_entries.reserve(fileCount);
        std::vector<std::string> folderOfEntry;
        folderOfEntry.reserve(fileCount);

        c.Seek(recordsStart);
        for (const auto& fr : folders) {
            if (fr.offset - fileNamesLen != c.Pos()) {
                Fail(err, "folder records out of canonical order — refusing to guess offsets");
                return std::nullopt;
            }
            std::uint8_t nameLen;
            if (!c.Read(nameLen) || nameLen == 0 || c.Remaining() < nameLen) {
                Fail(err, "bad folder name");
                return std::nullopt;
            }
            std::string folder(reinterpret_cast<const char*>(c.Here()), nameLen - 1u);
            c.Skip(nameLen);
            for (std::uint32_t i = 0; i < fr.count; ++i) {
                std::uint64_t hash;
                std::uint32_t size, offset;
                if (!c.Read(hash) || !c.Read(size) || !c.Read(offset)) {
                    Fail(err, "truncated file records");
                    return std::nullopt;
                }
                Entry e;
                e.offset = offset;
                e.size = size & kSizeMask;
                e.compressed = ((flags & kCompressed) != 0) != ((size & kSizeToggle) != 0);
                a.m_entries.push_back(std::move(e));
                folderOfEntry.push_back(folder);
            }
        }

        c.Seek(namesStart);
        const char* names = reinterpret_cast<const char*>(c.Here());
        std::size_t pos = 0;
        for (std::size_t i = 0; i < a.m_entries.size(); ++i) {
            const std::size_t len = ::strnlen(names + pos, fileNamesLen - pos);
            if (pos + len >= fileNamesLen) {
                Fail(err, "file name table shorter than the file count");
                return std::nullopt;
            }
            const std::string& folder = folderOfEntry[i];
            a.m_entries[i].path = folder.empty() ? std::string(names + pos, len)
                                                 : folder + '\\' + std::string(names + pos, len);
            pos += len + 1;
        }
        return a;
    }

    bool Archive::Extract(const Entry& e, std::vector<std::uint8_t>& out, std::string* err) const {
        std::ifstream f(m_path, std::ios::binary);
        if (!f) return Fail(err, "cannot open " + U8Path(m_path));
        std::vector<std::uint8_t> block(e.size);
        f.seekg(static_cast<std::streamoff>(e.offset));
        if (!f.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(block.size())))
            return Fail(err, "truncated data block: " + e.path);

        Cursor c(block.data(), block.size());
        if (m_flags & kEmbedNames) {
            std::uint8_t n;
            if (!c.Read(n) || !c.Skip(n)) return Fail(err, "bad embedded name: " + e.path);
        }

        if (!e.compressed) {
            out.assign(c.Here(), c.Here() + c.Remaining());
            return true;
        }

        std::uint32_t original;
        if (!c.Read(original)) return Fail(err, "missing original size: " + e.path);
        out.resize(original);

        LZ4F_dctx* raw = nullptr;
        if (LZ4F_isError(LZ4F_createDecompressionContext(&raw, LZ4F_VERSION)))
            return Fail(err, "LZ4F context creation failed");
        std::unique_ptr<LZ4F_dctx, decltype(&LZ4F_freeDecompressionContext)> ctx(raw, &LZ4F_freeDecompressionContext);

        const std::uint8_t* src = c.Here();
        std::size_t srcLeft = c.Remaining(), dstPos = 0, hint = 1;
        while (hint != 0) {
            std::size_t dstCap = out.size() - dstPos, srcUsed = srcLeft;
            hint = LZ4F_decompress(ctx.get(), out.data() + dstPos, &dstCap, src, &srcUsed, nullptr);
            if (LZ4F_isError(hint)) return Fail(err, std::string("LZ4F: ") + LZ4F_getErrorName(hint) + " in " + e.path);
            dstPos += dstCap;
            src += srcUsed;
            srcLeft -= srcUsed;
            if (dstCap == 0 && srcUsed == 0) break;
        }
        if (hint != 0 || dstPos != out.size())
            return Fail(err, "LZ4 frame truncated or size mismatch (" + std::to_string(dstPos) + " of " +
                                 std::to_string(out.size()) + " bytes): " + e.path);
        return true;
    }

}  // namespace vfs::bsa
