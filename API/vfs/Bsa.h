#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

/// @brief Read-only Bethesda archive (BSA) reader: enumerate + extract the files of an SSE/AE archive.
/// @note v105 only. v104 is Skyrim LE's format and SSE/AE does not load it, so it is rejected loudly rather
///   than carried as a dead path.
/// @warning Paths are returned exactly as the archive stores them — Bethesda's packer stores them LOWERCASED
///   with backslashes. Original case is not recoverable from a BSA (the case directive's hard limit):
///   never re-case them, and never normalize them further.
namespace vfs::bsa {

    struct Entry {
        std::string   path;           ///< archive-stored form, e.g. "meshes\actors\character\animations\1hm_idle.hkx"
        std::uint64_t offset = 0;
        std::uint32_t size = 0;       ///< on-disk block size, compression-toggle bit (0x40000000) already masked off
        bool          compressed = false;
    };

    class Archive {
    public:
        /// @return nullopt with @p err set on a missing file, bad magic, a non-v105 version, missing name tables,
        ///   or a directory whose records aren't in the canonical order (fail loud, never guess offsets).
        /// @note Canonical v105 layout: 36-byte header, folder records {hash u64, count u32, pad u32, offset u64},
        ///   then per folder a bzstring name (its length byte COUNTS the trailing NUL) followed by its file records
        ///   {hash u64, size u32, offset u32}, then the NUL-separated file-name table. A folder record's offset is
        ///   biased by the file-name table length; it is used only to verify that canonical order.
        [[nodiscard]] static std::optional<Archive> Open(const std::filesystem::path& path, std::string* err = nullptr);

        [[nodiscard]] const std::vector<Entry>& Entries() const noexcept { return m_entries; }
        [[nodiscard]] std::uint32_t             Version() const noexcept { return m_version; }

        /// @note Opens its own stream per call, so concurrent extraction from multiple threads is safe.
        /// @note Block layout: [bstring full path, NO trailing NUL — only when archive flag 0x100 is set], then
        ///   [u32 original size + LZ4 FRAME] when compressed, else the raw bytes. Size-field bit 0x40000000 inverts
        ///   the archive's default compression for that one file.
        [[nodiscard]] bool Extract(const Entry& e, std::vector<std::uint8_t>& out, std::string* err = nullptr) const;

    private:
        std::filesystem::path m_path;
        std::uint32_t         m_version = 0;
        std::uint32_t         m_flags = 0;
        std::vector<Entry>    m_entries;
    };

}  // namespace vfs::bsa
