#include <vfs/Vfs.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

void Write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string Text(const std::optional<std::vector<std::uint8_t>>& bytes) {
    return bytes ? std::string(bytes->begin(), bytes->end()) : std::string("<none>");
}

/// @brief Two folders: the higher one wins a shared path, lookups fold case and slashes, spellings are kept.
void LooseLayers(const fs::path& tmp) {
    Write(tmp / "low/Meshes/Actors/A.nif", "low");
    Write(tmp / "low/meshes/b.nif", "only-low");
    Write(tmp / "high/meshes/actors/a.nif", "high");
    vfs::FileSystem v;
    vfs::Options    o;
    o.kind    = vfs::Options::Kind::Folders;
    o.folders = { tmp / "low", tmp / "high" };
    std::string err;
    Check(v.Open(o, &err), "open folders");
    Check(Text(v.Read("MESHES\\Actors\\a.NIF")) == "high", "higher folder wins, case and slashes fold");
    Check(Text(v.Read("meshes/b.nif")) == "only-low", "a file only the lower folder has");
    Check(v.Providers("meshes/actors/a.nif").size() == 2, "both providers listed");
    Check(v.Winner("meshes/actors/a.nif")->layer == "folder 'high'", "winner is the higher folder");
    const auto list = v.List("Meshes");
    Check(list.size() == 2, "two files below meshes/");
    Check(std::find(list.begin(), list.end(), "meshes/actors/a.nif") != list.end(), "listed as the winner spells it");
    Check(!v.Exists("meshes/c.nif"), "an absent file");
}

/// @brief A fake MO2 instance: gamePath from ModOrganizer.ini, modlist.txt's highest-first order, separators and
///        disabled mods skipped, overwrite on top.
void Mo2Instance(const fs::path& tmp) {
    const fs::path inst = tmp / "mo2";
    Write(inst / "ModOrganizer.ini", "[General]\ngamePath=@ByteArray(" + (tmp / "game").string() + ")\n");
    Write(tmp / "game/Data/Skyrim.esm", "esm");
    Write(tmp / "game/Data/x.txt", "game");
    Write(inst / "profiles/Default/modlist.txt", "# header\n+Top\n-Off\n+Group_separator\n+Bottom\n*DLC: Dawnguard\n");
    Write(inst / "profiles/Default/plugins.txt", "*Skyrim.esm\n");
    Write(inst / "mods/Bottom/x.txt", "bottom");
    Write(inst / "mods/Top/x.txt", "top");
    Write(inst / "mods/Off/x.txt", "off");
    Write(inst / "overwrite/y.txt", "overwrite");
    std::string err;
    const auto  p = vfs::ReadMo2(inst, "", &err);
    Check(p.has_value(), "read the MO2 instance");
    if (!p) return std::printf("  %s\n", err.c_str()), void();
    Check(p->mods.size() == 2 && p->mods[0] == "Bottom" && p->mods[1] == "Top", "enabled mods, lowest priority first");
    vfs::FileSystem v;
    vfs::Options    o;
    o.kind        = vfs::Options::Kind::Mo2;
    o.mo2Instance = inst;
    Check(v.Open(o, &err), "open the MO2 instance");
    Check(Text(v.Read("x.txt")) == "top", "the highest enabled mod wins");
    Check(Text(v.Read("y.txt")) == "overwrite", "overwrite is read");
    Check(v.LoosePath("skyrim.esm").has_value(), "a plugin in the game's Data is loose");
    Check(v.Layers().size() == 4, "game Data, two mods, overwrite");
}

/// @brief Files held in memory, as a tool's overlay would.
class MemoryOverlay final : public vfs::Overlay {
public:
    std::map<std::string, std::string> files;
    std::vector<std::string> Paths() const override {
        std::vector<std::string> out;
        for (const auto& [p, text] : files) out.push_back(p);
        return out;
    }
    std::optional<std::vector<std::uint8_t>> Read(std::string_view path, std::string* err) const override {
        const auto it = files.find(std::string(path));
        if (it == files.end()) {
            if (err) *err = "gone";
            return std::nullopt;
        }
        return std::vector<std::uint8_t>(it->second.begin(), it->second.end());
    }
};

/// @brief Overlays win over loose files, the later-set one wins, and they come and go without re-opening.
void Overlays(const fs::path& tmp) {
    Write(tmp / "ov/meshes/a.nif", "loose");
    Write(tmp / "ov/meshes/b.nif", "loose-b");
    vfs::FileSystem v;
    vfs::Options    o;
    o.kind    = vfs::Options::Kind::Folders;
    o.folders = { tmp / "ov" };
    Check(v.Open(o), "open for overlays");
    auto lower = std::make_shared<MemoryOverlay>();
    lower->files["Meshes/A.nif"]   = "lower";
    lower->files["meshes/new.nif"] = "new";
    auto upper = std::make_shared<MemoryOverlay>();
    upper->files["meshes/a.nif"] = "upper";
    v.SetOverlay("lower", lower);
    v.SetOverlay("upper", upper);
    Check(Text(v.Read("MESHES/a.nif")) == "upper", "the later overlay wins");
    Check(Text(v.Read("meshes/new.nif")) == "new", "an overlay adds a file");
    Check(Text(v.Read("meshes/b.nif")) == "loose-b", "loose files show through");
    Check(!v.LoosePath("meshes/a.nif").has_value(), "an overlaid file has no loose path");
    Check(v.Providers("meshes/a.nif").size() == 3 && v.Winner("meshes/a.nif")->layer == "upper" && v.Winner("meshes/a.nif")->overlay,
          "providers: loose, lower, upper");
    Check(v.List("meshes").size() == 3, "listing merges overlays");
    lower->files["meshes/Sub/deep.nif"] = "deep";
    v.RefreshOverlay("lower");
    const auto top = v.Children("");
    Check(top.size() == 1 && top[0].folder && top[0].name == "meshes", "one folder at the root");
    const auto below = v.Children("MESHES");
    Check(below.size() == 4 && below[0].folder && below[0].name == "Sub" && !below[1].folder, "a sub-folder, then the files");
    Check(v.Children("meshes/sub").size() == 1, "a folder an overlay supplies");
    v.SetOverlay("lower", lower);
    Check(Text(v.Read("meshes/a.nif")) == "upper", "replacing an overlay keeps its rank");
    upper->files["meshes/c.nif"] = "c";
    Check(!v.Exists("meshes/c.nif"), "an overlay's new file needs a refresh");
    v.RefreshOverlay("upper");
    Check(Text(v.Read("meshes/c.nif")) == "c", "refreshed");
    Check(v.Open(o) && Text(v.Read("meshes/c.nif")) == "c", "overlays survive a re-open");
    v.RemoveOverlay("upper");
    Check(Text(v.Read("meshes/a.nif")) == "lower", "removing an overlay uncovers the one below");
    v.RemoveOverlay("lower");
    Check(Text(v.Read("meshes/a.nif")) == "loose" && !v.Exists("meshes/new.nif"), "all gone: back to loose");
}

/// @brief VFS_TEST_MO2 (a real MO2 instance) or VFS_TEST_DATA (a real Skyrim SE Data folder), optional: the vanilla
///        archives load and serve a known file.
void RealData() {
    const char*  mo2  = std::getenv("VFS_TEST_MO2");
    const char*  data = std::getenv("VFS_TEST_DATA");
    if (!mo2 && !data) return;
    vfs::FileSystem v;
    vfs::Options    o;
    if (mo2) o.kind = vfs::Options::Kind::Mo2, o.mo2Instance = mo2;
    else o.dataFolder = data;
    std::string err;
    Check(v.Open(o, &err), "open the real Data folder");
    std::printf("  real Data: %zu layer(s)\n", v.Layers().size());
    const std::size_t n = v.LoadArchives({ "Skyrim.esm", "Update.esm", "Dawnguard.esm" });
    std::printf("  real Data: %zu archive(s) opened, %zu warning(s)\n", n, v.Warnings().size());
    const auto skel = v.Read("meshes/actors/character/character assets/skeleton.hkx", &err);
    Check(skel && skel->size() > 1000, "the humanoid skeleton reads out of an archive");
}

}  // namespace

int main() {
    const fs::path tmp = fs::temp_directory_path() / "vfs-test";
    fs::remove_all(tmp);
    LooseLayers(tmp);
    Mo2Instance(tmp);
    Overlays(tmp);
    RealData();
    fs::remove_all(tmp);
    std::printf("%s: vfs-test, %d failure(s)\n", g_failures ? "FAIL" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
