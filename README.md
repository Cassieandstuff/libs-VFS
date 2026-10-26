# VFS

Skyrim's Data folder as the game sees it: loose files over BSA archives, assembled from a game Data folder, a
Mod Organizer 2 instance + profile, or plain folders. C++23; depends on lz4.

```cmake
FetchContent_Declare(VFS SOURCE_DIR D:/Projects/Common/internal/VFS)   # or a git repository + tag
FetchContent_MakeAvailable(VFS)
target_link_libraries(MyTool PRIVATE vfs::vfs)
```

```cpp
#include <vfs/Vfs.h>

vfs::FileSystem data;
vfs::Options    opt;
opt.kind        = vfs::Options::Kind::Mo2;
opt.mo2Instance = R"(D:\MO2 Data\SSE - Dragon Break Project)";   // portable folder, or a global instance's data folder
std::string err;
if (!data.Open(opt, &err)) return fail(err);
data.LoadArchives(pluginsInLoadOrder);                           // e.g. from the profile's plugins.txt (data.Mo2()->pluginsTxt)
auto skeleton = data.Read("meshes/actors/character/character assets/skeleton.hkx");
```

| Call | What |
|---|---|
| `Open` | find the layers (game Data, enabled mods lowest priority first, overwrite) and index their loose files |
| `LoadArchives` | the ini archive lists, then each plugin's `<stem>.bsa` / `<stem> - Textures.bsa`, in load order |
| `Read` / `Exists` / `LoosePath` | the winning version of a Data-relative path |
| `List` | every file below a folder |
| `Winner` / `Providers` | where a file comes from, and everything it overrides |

Precedence: any loose file beats every archive; among loose files the higher layer wins; among archives the later one.
Lookups fold case and slashes (the engine's own path identity); returned paths keep their source's spelling.

## Build and test

```bash
cmake -S . -B build -Dlz4_DIR=<lz4's cmake package dir>      # or a vcpkg toolchain (vcpkg.json lists lz4)
cmake --build build --config Release
ctest --test-dir build -C Release
```

`VFS_TEST_MO2=<instance>` or `VFS_TEST_DATA=<Data folder>` adds a check against a real install.
