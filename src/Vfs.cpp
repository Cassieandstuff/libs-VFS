#include <vfs/Vfs.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <unordered_map>

namespace vfs {

namespace fs = std::filesystem;

namespace {

std::optional<std::string> ReadText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

/// @brief ModOrganizer.ini's value for `key` (any section), unwrapped from @ByteArray(...) and unescaped.
std::optional<std::string> IniValue(const std::string& ini, std::string_view key) {
    std::istringstream lines(ini);
    for (std::string line; std::getline(lines, line);) {
        line = Trim(line);
        if (line.size() <= key.size() || line.compare(0, key.size(), key) != 0 || line[key.size()] != '=') continue;
        std::string v = line.substr(key.size() + 1);
        if (v.rfind("@ByteArray(", 0) == 0 && !v.empty() && v.back() == ')') v = v.substr(11, v.size() - 12);
        std::string out;
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == '\\') ++i;
            out += v[i];
        }
        return out;
    }
    return std::nullopt;
}

/// @brief `p` as UTF-8 text, forward slashes.
/// @note  path::string() throws on Windows for a name outside the ANSI code page (mod files carry such names), so
///        every path crosses into text as UTF-8.
std::string U8(const fs::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}
/// @brief UTF-8 text as a path.
fs::path P(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())); }

fs::path Expand(std::string v, const fs::path& base) {
    const std::string token = "%BASE_DIR%";
    if (const auto at = v.find(token); at != std::string::npos) v.replace(at, token.size(), U8(base));
    return P(v);
}

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::string Slashed(std::string_view s) {
    std::string out(s);
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

}  // namespace

const std::vector<std::string>& DefaultIniArchives() {
    static const std::vector<std::string> kList{
        "Skyrim - Misc.bsa",     "Skyrim - Shaders.bsa",   "Skyrim - Interface.bsa", "Skyrim - Animations.bsa",
        "Skyrim - Meshes0.bsa",  "Skyrim - Meshes1.bsa",   "Skyrim - Sounds.bsa",    "Skyrim - Voices_en0.bsa",
        "Skyrim - Textures0.bsa", "Skyrim - Textures1.bsa", "Skyrim - Textures2.bsa", "Skyrim - Textures3.bsa",
        "Skyrim - Textures4.bsa", "Skyrim - Textures5.bsa", "Skyrim - Textures6.bsa", "Skyrim - Textures7.bsa",
        "Skyrim - Textures8.bsa", "Skyrim - Patch.bsa",
    };
    return kList;
}

std::optional<Mo2Profile> ReadMo2(const fs::path& instance, std::string profile, std::string* err) {
    const auto fail = [&](std::string why) -> std::optional<Mo2Profile> {
        if (err) *err = std::move(why);
        return std::nullopt;
    };
    /// @note A portable instance keeps ModOrganizer.ini in its own folder; a global one keeps it in
    ///       %LOCALAPPDATA%/ModOrganizer/<instance name>/, with base_directory naming the data folder.
    std::optional<std::string> found = ReadText(instance / "ModOrganizer.ini");
    if (!found)
        if (const char* local = std::getenv("LOCALAPPDATA"))
            found = ReadText(fs::path(local) / "ModOrganizer" / instance.filename() / "ModOrganizer.ini");
    const std::string ini  = found.value_or(std::string());
    const fs::path    base = IniValue(ini, "base_directory") ? Expand(*IniValue(ini, "base_directory"), instance) : instance;
    if (profile.empty()) profile = IniValue(ini, "selected_profile").value_or(std::string());
    const auto        dir  = [&](std::string_view key, const char* fallback) {
        const auto v = IniValue(ini, key);
        return v ? Expand(*v, base) : base / fallback;
    };
    Mo2Profile p;
    p.instance  = instance;
    p.modsDir   = dir("mod_directory", "mods");
    p.overwrite = dir("overwrite_directory", "overwrite");
    const fs::path profiles = dir("profiles_directory", "profiles");
    const auto     game     = IniValue(ini, "gamePath");
    p.gameData = (game ? P(*game) : instance / "Game") / "Data";

    std::error_code ec;
    if (profile.empty()) {
        if (fs::is_directory(profiles / "Default", ec)) profile = "Default";
        else
            for (fs::directory_iterator it(profiles, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_directory(ec)) { profile = U8(it->path().filename()); break; }
    }
    if (profile.empty()) return fail("no profile under " + U8(profiles));
    p.profile    = profile;
    p.pluginsTxt = profiles / P(profile) / "plugins.txt";
    const auto modlist = ReadText(profiles / P(profile) / "modlist.txt");
    if (!modlist) return fail("no modlist.txt in profile '" + profile + "' under " + U8(profiles));

    /// @note modlist.txt lists the HIGHEST priority first: `+` enabled, `-` disabled, `*` unmanaged (game files).
    std::vector<std::string> enabled;
    std::istringstream       lines(*modlist);
    for (std::string line; std::getline(lines, line);) {
        line = Trim(line);
        if (line.size() < 2 || line[0] != '+') continue;
        const std::string name = line.substr(1);
        if (name.size() > 10 && name.compare(name.size() - 10, 10, "_separator") == 0) continue;
        enabled.push_back(name);
    }
    p.mods.assign(enabled.rbegin(), enabled.rend());
    return p;
}

struct FileSystem::Impl {
    struct Hit {
        std::size_t source = 0;   ///< layer index (loose) or archive index (archived)
        std::size_t entry  = 0;   ///< archive entry index; unused for loose
        std::string spelled;      ///< the path as its source spells it, forward slashes
    };
    struct Archive {
        std::string  name;
        std::string  layer;
        bsa::Archive archive;
    };

    std::vector<Layer>                                 layers;
    std::optional<Mo2Profile>                          mo2;
    std::vector<std::string>                           warnings;
    std::unordered_map<std::string, std::vector<Hit>> loose;      ///< key -> hits, lowest layer first
    std::vector<Archive>                               archives;   ///< load order
    std::unordered_map<std::string, std::vector<Hit>> archived;   ///< key -> hits, load order
    struct Overlaid {
        std::string                                  name;
        std::shared_ptr<const Overlay>               overlay;
        std::unordered_map<std::string, std::string> paths;   ///< key -> its spelling
    };
    std::vector<Overlaid> overlays;   ///< lowest first

    /// @brief The highest overlay supplying `key`, or null.
    const Overlaid* OverlayOf(const std::string& key) const {
        for (auto it = overlays.rbegin(); it != overlays.rend(); ++it)
            if (it->paths.count(key)) return &*it;
        return nullptr;
    }

    static void Reindex(Overlaid& o) {
        o.paths.clear();
        if (o.overlay)
            for (auto& p : o.overlay->Paths()) {
                std::string k = Key(p);
                o.paths.emplace(std::move(k), Slashed(p));
            }
    }

    void Index(std::size_t layer) {
        std::error_code ec;
        const fs::path& root = layers[layer].root;
        for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string rel = U8(it->path().lexically_relative(root));
            loose[Key(rel)].push_back({ layer, 0, rel });
        }
        if (ec) warnings.push_back("cannot fully walk " + U8(root) + ": " + ec.message());
    }
};

FileSystem::FileSystem() : m_impl(std::make_unique<Impl>()) {}
FileSystem::~FileSystem()                                = default;
FileSystem::FileSystem(FileSystem&&) noexcept            = default;
FileSystem& FileSystem::operator=(FileSystem&&) noexcept = default;

std::string FileSystem::Key(std::string_view path) {
    std::string k = Lower(Slashed(path));
    while (!k.empty() && k.front() == '/') k.erase(k.begin());
    return k;
}

bool FileSystem::Open(const Options& o, std::string* err) {
    /// @note Overlays are not part of what is opened: they survive a re-Open.
    auto overlays    = std::move(m_impl->overlays);
    *m_impl          = Impl{};
    m_impl->overlays = std::move(overlays);
    auto&        L   = m_impl->layers;
    switch (o.kind) {
    case Options::Kind::DataFolder: L.push_back({ "game Data", o.dataFolder }); break;
    case Options::Kind::Folders:
        for (const auto& f : o.folders) L.push_back({ "folder '" + U8(f.filename()) + "'", f });
        break;
    case Options::Kind::Mo2: {
        auto p = ReadMo2(o.mo2Instance, o.mo2Profile, err);
        if (!p) return false;
        L.push_back({ "game Data", p->gameData });
        for (const auto& m : p->mods) L.push_back({ "mod '" + m + "'", p->modsDir / P(m) });
        L.push_back({ "overwrite", p->overwrite });
        m_impl->mo2 = std::move(*p);
        break;
    }
    }
    std::error_code ec;
    std::erase_if(L, [&](const Layer& l) {
        const bool missing = !fs::is_directory(l.root, ec);
        if (missing) m_impl->warnings.push_back(l.name + ": no folder " + U8(l.root));
        return missing;
    });
    if (L.empty()) {
        if (err) *err = "no Data folder to read";
        return false;
    }
    for (std::size_t i = 0; i < L.size(); ++i) m_impl->Index(i);
    return true;
}

std::size_t FileSystem::LoadArchives(const std::vector<std::string>& pluginsInOrder, const std::vector<std::string>& iniArchives) {
    auto& I = *m_impl;
    I.archives.clear();
    I.archived.clear();
    std::vector<std::string> names(iniArchives);
    /// @note The engine loads `<plugin>.bsa` and `<plugin> - Textures.bsa` for each loaded plugin, in load order.
    for (const auto& plugin : pluginsInOrder) {
        const std::string stem = U8(P(plugin).stem());
        names.push_back(stem + ".bsa");
        names.push_back(stem + " - Textures.bsa");
    }
    std::set<std::string> seen;
    for (const auto& name : names) {
        if (!seen.insert(Key(name)).second) continue;
        const auto hit = I.loose.find(Key(name));
        if (hit == I.loose.end()) continue;
        const Impl::Hit& w = hit->second.back();
        std::string      why;
        auto             a = bsa::Archive::Open(I.layers[w.source].root / P(w.spelled), &why);
        if (!a) {
            I.warnings.push_back(name + ": " + why);
            continue;
        }
        const std::size_t index = I.archives.size();
        I.archives.push_back({ U8(P(w.spelled).filename()), I.layers[w.source].name, std::move(*a) });
        const auto& entries = I.archives.back().archive.Entries();
        for (std::size_t e = 0; e < entries.size(); ++e) I.archived[Key(entries[e].path)].push_back({ index, e, Slashed(entries[e].path) });
    }
    return I.archives.size();
}

const std::vector<Layer>&       FileSystem::Layers() const { return m_impl->layers; }
const std::vector<std::string>& FileSystem::Warnings() const { return m_impl->warnings; }
const Mo2Profile*               FileSystem::Mo2() const { return m_impl->mo2 ? &*m_impl->mo2 : nullptr; }

void FileSystem::SetOverlay(const std::string& name, std::shared_ptr<const Overlay> overlay) {
    auto&      O  = m_impl->overlays;
    const auto it = std::find_if(O.begin(), O.end(), [&](const Impl::Overlaid& o) { return o.name == name; });
    Impl::Overlaid& o = it != O.end() ? *it : O.emplace_back();
    o.name            = name;
    o.overlay         = std::move(overlay);
    Impl::Reindex(o);
}

void FileSystem::RemoveOverlay(const std::string& name) {
    std::erase_if(m_impl->overlays, [&](const Impl::Overlaid& o) { return o.name == name; });
}

void FileSystem::RefreshOverlay(const std::string& name) {
    for (auto& o : m_impl->overlays)
        if (o.name == name) Impl::Reindex(o);
}

bool FileSystem::Exists(std::string_view path) const {
    const std::string k = Key(path);
    return m_impl->OverlayOf(k) || m_impl->loose.count(k) || m_impl->archived.count(k);
}

std::optional<fs::path> FileSystem::LoosePath(std::string_view path) const {
    if (m_impl->OverlayOf(Key(path))) return std::nullopt;
    const auto it = m_impl->loose.find(Key(path));
    if (it == m_impl->loose.end()) return std::nullopt;
    const Impl::Hit& w = it->second.back();
    return m_impl->layers[w.source].root / P(w.spelled);
}

std::optional<std::vector<std::uint8_t>> FileSystem::Read(std::string_view path, std::string* err) const {
    if (const auto k = Key(path); const Impl::Overlaid* o = m_impl->OverlayOf(k))
        return o->overlay->Read(o->paths.at(k), err);
    if (const auto p = LoosePath(path)) {
        std::ifstream f(*p, std::ios::binary);
        if (!f) {
            if (err) *err = "cannot open " + U8(*p);
            return std::nullopt;
        }
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
    }
    const auto it = m_impl->archived.find(Key(path));
    if (it == m_impl->archived.end()) {
        if (err) *err = "not in the Data folder: " + std::string(path);
        return std::nullopt;
    }
    const Impl::Hit&          w = it->second.back();
    const auto&               a = m_impl->archives[w.source].archive;
    std::vector<std::uint8_t> out;
    if (!a.Extract(a.Entries()[w.entry], out, err)) return std::nullopt;
    return out;
}

std::vector<std::string> FileSystem::List(std::string_view prefix) const {
    std::string p = Key(prefix);
    if (!p.empty() && p.back() != '/') p += '/';
    std::map<std::string, std::string> out;   ///< key -> the winner's spelling
    for (const auto& [k, hits] : m_impl->archived)
        if (k.starts_with(p)) out[k] = hits.back().spelled;
    for (const auto& [k, hits] : m_impl->loose)
        if (k.starts_with(p)) out[k] = hits.back().spelled;
    for (const auto& o : m_impl->overlays)
        for (const auto& [k, spelled] : o.paths)
            if (k.starts_with(p)) out[k] = spelled;
    std::vector<std::string> list;
    list.reserve(out.size());
    for (auto& [k, spelled] : out) list.push_back(std::move(spelled));
    std::sort(list.begin(), list.end());
    return list;
}

std::vector<Provider> FileSystem::Providers(std::string_view path) const {
    const std::string     k = Key(path);
    std::vector<Provider> out;
    if (const auto it = m_impl->archived.find(k); it != m_impl->archived.end())
        for (const auto& h : it->second) out.push_back({ m_impl->archives[h.source].layer, m_impl->archives[h.source].name, {} });
    if (const auto it = m_impl->loose.find(k); it != m_impl->loose.end())
        for (const auto& h : it->second) out.push_back({ m_impl->layers[h.source].name, {}, m_impl->layers[h.source].root / P(h.spelled) });
    for (const auto& o : m_impl->overlays)
        if (o.paths.count(k)) out.push_back({ o.name, {}, {}, true });
    return out;
}

std::optional<Provider> FileSystem::Winner(std::string_view path) const {
    auto all = Providers(path);
    if (all.empty()) return std::nullopt;
    return all.back();
}

}  // namespace vfs
