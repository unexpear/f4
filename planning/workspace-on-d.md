# Workspace on D:

## Active Location

Use `D:\dev\f4` as the project root, working directory, and editor project.
The source and CMake presets contain no hard-coded dependency on the previous
project location. Git now discovers its worktree normally; the temporary local
`core.worktree` override has been removed.

The saved Codex project was rechecked and now points to `D:\dev\f4`.
Use that project for subsequent work, not the old compatibility folder.

## Builds

Run from the D: root:

```powershell
Set-Location 'D:\dev\f4'
cmake --preset release
cmake --build --preset build-release --parallel 4
ctest --preset test-release

cmake --preset msvc-asan
cmake --build --preset build-msvc-asan --target f4_tests f4_tests_stripped --parallel 2
ctest --preset test-msvc-asan

$env:EMSDK = 'C:\Users\micha\.codex\toolchains\emsdk'
cmake --preset webgpu-debug
cmake --build --preset build-webgpu-debug --parallel 2
ctest --preset test-webgpu-debug
```

The shared compiler, CMake, SDL2, Vulkan SDK, and Emscripten installations remain
on C:. These are tools, not links to the old project. Build outputs are on D:.
For temporary compiler/browser files on D:, create `build\tmp` if needed and set
`TEMP` and `TMP` to its absolute path in the build shell. This does not change
the user's global environment.

## Migration Changes

- Release was regenerated using CMake `--fresh` with the local cache setting
  `CMAKE_POLICY_DEFAULT_CMP0168=NEW`, then rebuilt. This uses the documented direct
  FetchContent implementation instead of stale dependency sub-builds.
- MSVC ASan and WebGPU were configured in fresh directories and rebuilt.
- The regenerated active CMake caches, Ninja files, compiler command databases,
  Visual Studio projects, and test scripts were checked for the old C: project
  path. Archived files and historical test logs are not active build inputs.
- Old Debug, GCC ASan, MSVC Debug/Release, Linux, and selected Vulkan/DX12 build
  directories were renamed with `.c-drive-archive` suffixes under `build`.
  Reconfigure the desired preset from D: before using those configurations again.
  Linux and macOS were not rebuilt or accepted as part of this migration.
- Obsolete Release dependency sub-builds and disabled Alembic test files were
  archived similarly. Existing visual capture galleries were preserved.
- No engine source, dependency pins, sanitizer exclusions, or assets changed.

## Verification - 2026-09-05

- Release: full build succeeded; CTest 8/8 passed, including static and streaming
  screenshot gates (26.96 seconds).
- MSVC ASan: fresh test binaries built; CTest 6/6 passed (6.45 seconds), retaining
  the existing native-driver exclusions.
- WebGPU Debug: fresh Emscripten build succeeded; browser smoke 1/1 passed
  (12.82 seconds).
- Final active generated-file path audit passed for all three configurations.
  All three caches identify `D:/dev/f4` as `CMAKE_HOME_DIRECTORY`.
- Git reports `D:/dev/f4` without a `core.worktree` override. Existing modified
  and untracked entries were preserved; this note is the only added repo file.
  `git diff --check` passed.
- The migration used documented [CMake fresh configuration](https://cmake.org/cmake/help/latest/manual/cmake.1.html#cmdoption-cmake-fresh)
  and [CMP0168 behavior](https://cmake.org/cmake/help/latest/policy/CMP0168.html),
  not search-and-replace edits to generated binary build caches.

## Cleanup Status

Cleanup was verified after the user's manual deletion. Both the old
`C:\Users\micha\Documents\code projects\f4` compatibility folder and
`C:\Users\micha\Documents\code projects\f4-migration-old-20260905` backup
are absent. The active project remains at `D:\dev\f4`.

Tests were rerun with both old paths physically absent: Release 8/8 (23.89
seconds), MSVC ASan 6/6 (6.60 seconds), and WebGPU browser smoke 1/1 (12.68
seconds). The Release run includes both screenshot gates. All 2,838 asset files
matched their pre-migration SHA-256 hashes. Existing Git changes are preserved;
the only additional repository entry is this note.

At this check, C: had 14.32 GiB free and D: had 173.56 GiB free. The complete
pre-cutover SHA-256 manifest and migration records remain in
`D:\dev\f4-migration-20260905`.
