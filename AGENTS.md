## project

**memview** is a lightweight process memory scanner/editor for Windows (x64), written in
C++23. It attaches to a running process you own and lets you scan, view, and edit its
memory live.

Core features: value scanning (first/next), a saved address list, hex view and
disassembly, a memory region map, an inline assembler/disassembler (asmjit + Zydis),
and AOB signature generation.

GUI is Dear ImGui (Win32 + DirectX 11). Built with CMake + Ninja + Clang, dependencies
via vcpkg.

## layout

- `src/main.cpp` - entry point
- `src/app/` - app lifecycle, config, window and rendering
- `src/memory/` - scanning, signatures, assembler, value formatting
- `src/ui/` - ImGui panels (scan, address list, hex view, disasm, regions, toolbar, process picker)
- `src/assets/` - fonts and icons
- `external/` - git submodules

## build

```sh
cmake --preset release
cmake --build --preset release
```

## conventions

- Comments are minimal and human - write them only where intent isn't obvious, in plain
  language. No obvious restatements of the code, no noise.
- Match the style and idioms of the surrounding code.
- Always use [Conventional Commits](https://www.conventionalcommits.org) for commit
  messages.
