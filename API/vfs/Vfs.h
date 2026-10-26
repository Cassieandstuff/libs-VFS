#pragma once
#include <vfs/Bsa.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// @brief VFS — Skyrim's Data folder as the game sees it: loose files over BSA archives, assembled from a game Data
///        folder, a Mod Organizer 2 instance + profile, or plain folders. The one place tools resolve a Data-relative
///        path (a plugin, a string table, a mesh, a .hky bundle).
/// @note  Path identity folds case and slash direction, as the engine's own resource lookup does; every returned path
///        keeps its source's spelling. Nothing here re-cases a name.

namespace vfs {

/// @brief One loose-file root laid out like a Data folder: the game's Data, a mod, MO2's overwrite, a folder.
struct Layer {
    std::string           name;   ///< for display ("game Data", "mod 'Foo'", "overwrite")
    std::filesystem::path root;
};

/// @brief Where a file comes from: a layer's loose file, or an archive (named by file) a layer holds.
struct Provider {
    std::string           layer;     ///< Layer::name
    std::string           archive;   ///< the .bsa's file name; empty for a loose file
    std::filesystem::path loose;     ///< the loose file on disk; empty for an archived one
};

/// @brief A Mod Organizer 2 instance's view: the game's Data, the profile's enabled mods by priority, overwrite.
struct Mo2Profile {
    std::filesystem::path    instance;
    std::string              profile;
    std::filesystem::path    gameData;
    std::filesystem::path    modsDir;
    std::filesystem::path    overwrite;
    std::filesystem::path    pluginsTxt;   ///< the profile's plugins.txt (the load order the game gets)
    std::vector<std::string> mods;         ///< enabled mods, LOWEST priority first
};

/// @brief Read an MO2 instance (its own folder, or a global instance's data folder / name): ModOrganizer.ini's
///        directories and gamePath (else `<instance>/Game`), the profile's modlist.txt.
/// @param profile empty = the ini's selected_profile, else "Default" when present, else the first profile folder.
/// @return nullopt with `err` set when the instance or its profile cannot be read
std::optional<Mo2Profile> ReadMo2(const std::filesystem::path& instance, std::string profile, std::string* err = nullptr);

/// @brief What a FileSystem is built over.
struct Options {
    enum class Kind { DataFolder, Mo2, Folders } kind = Kind::DataFolder;
    std::filesystem::path              dataFolder;    ///< Kind::DataFolder
    std::filesystem::path              mo2Instance;   ///< Kind::Mo2
    std::string                        mo2Profile;    ///< Kind::Mo2; empty = ReadMo2's default
    std::vector<std::filesystem::path> folders;       ///< Kind::Folders, LOWEST priority first
};

/// @brief Skyrim SE/AE's default archive lists (Skyrim.ini [Archive] sResourceArchiveList + sResourceArchiveList2),
///        which load before any plugin's archives.
const std::vector<std::string>& DefaultIniArchives();

/// @brief The assembled Data folder.
/// @note  Precedence: any loose file beats every archive; among loose files the higher layer wins; among archives the
///        later-loaded wins (the ini lists first, then each plugin's in load order). Not thread-safe while building;
///        Read is safe from several threads once built.
class FileSystem {
public:
    FileSystem();
    ~FileSystem();
    FileSystem(FileSystem&&) noexcept;
    FileSystem& operator=(FileSystem&&) noexcept;

    /// @brief Find the layers and index every loose file in them.
    /// @return false with `err` set when no layer exists
    bool Open(const Options& options, std::string* err = nullptr);

    /// @brief Open the archives in load order: `iniArchives`, then for each plugin (file names in load order, e.g.
    ///        from plugins.txt) `<stem>.bsa` and `<stem> - Textures.bsa` (what the engine loads), each found as a loose
    ///        file of the assembled Data.
    /// @return the archives opened; one that fails to open is skipped and reported in Warnings()
    std::size_t LoadArchives(const std::vector<std::string>& pluginsInOrder,
                             const std::vector<std::string>& iniArchives = DefaultIniArchives());

    const std::vector<Layer>&       Layers() const;
    const std::vector<std::string>& Warnings() const;
    /// @brief The MO2 profile it was opened over, or null.
    const Mo2Profile* Mo2() const;

    bool Exists(std::string_view path) const;
    /// @brief The winning version of `path` (Data-relative, either slash, any case).
    std::optional<std::vector<std::uint8_t>> Read(std::string_view path, std::string* err = nullptr) const;
    /// @brief The winning version's loose file on disk, or nullopt when it is archived or absent (a plugin is always
    ///        loose).
    std::optional<std::filesystem::path> LoosePath(std::string_view path) const;
    /// @brief Every file below `prefix` (a Data-relative folder; empty = all), as its winner spells it, sorted.
    std::vector<std::string> List(std::string_view prefix) const;
    /// @brief The winner of `path`, or nullopt.
    std::optional<Provider> Winner(std::string_view path) const;
    /// @brief Every provider of `path`, LOWEST precedence first (the winner last).
    std::vector<Provider> Providers(std::string_view path) const;

    /// @brief `path` as the index keys it: lowercase ASCII, forward slashes, no leading slash.
    static std::string Key(std::string_view path);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace vfs
