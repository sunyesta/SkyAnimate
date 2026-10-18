# SkyAnimate

A 2D animation app inspired by Moho and Blender: Moho's animation model (layers, bones, smart
bones, actions) inside Blender's interface and editing habits. Windows only for now.

See [docs/SPEC.md](docs/SPEC.md) for the feature spec and roadmap, and [CLAUDE.md](CLAUDE.md) for
build instructions and project conventions.

## Quick start

```
cmake --preset windows
cmake --build --preset release
build\bin\RelWithDebInfo\SkyAnimate.exe
```

Requires Visual Studio 2026 Build Tools (MSVC and its bundled CMake) and Python 3.14 at
`C:\Python314`. Other libraries are downloaded by CMake.
