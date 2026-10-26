# VFS — working notes

Skyrim's Data folder as the game sees it (loose over BSA; game Data / MO2 instance + profile / folders). The one Data
layer Behavior Kit's load-order documents (ESPER's plugins, Community Behaviors' .hky bundles) and asset loading read
through. Consumers link `vfs::vfs` and include `<vfs/Vfs.h>` only.

## Rules
- Paths cross into text as UTF-8 (`generic_u8string`): `path::string()` throws on Windows for names outside the ANSI
  code page, and mod folders carry such names.
- Path identity (`FileSystem::Key`) folds ASCII case and slashes, as the engine's resource lookup does. Never re-case a
  returned path: it keeps its source's spelling (an archive stores lowercase; loose files keep theirs).
- Archives: SSE/AE v105 only (`vfs::bsa`); v104 is Skyrim LE and is refused loudly.
- Never set `CMAKE_MSVC_RUNTIME_LIBRARY`: it inherits the consumer's (static-CRT SKSE plugins, dynamic-CRT Qt apps).
- Comments are Doxygen annotations only (`///`), carrying the why, contract or gotcha, never a restatement.
