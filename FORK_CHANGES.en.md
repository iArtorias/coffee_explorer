# Coffee Explorer — Fork Changes

> Language: **English** · [简体中文](FORK_CHANGES.md)

This repository is based on upstream [iArtorias/coffee_explorer](https://github.com/iArtorias/coffee_explorer) **v1.0.0**, with two local improvements that leave the original architecture and behaviour intact.

| Item | Value |
|---|---|
| Upstream version | Coffee Explorer 1.0.0 |
| Changes in this fork | ① Drag and drop reordering of the import directory　② Simplified Chinese language file |
| Date | 2026-11-10 |
| License | GNU GPL v3 (same as upstream; the modifications in this fork are released under GPLv3 as well) |

---

## Summary

| # | Change | Scope | New files |
|:--:|---|---|---|
| 1 | Reorder import descriptors by dragging | `core/src/Operations.cpp`, `ui/CoffeeExplorer/Controls/TableView.cs` | none |
| 2 | Simplified Chinese interface | `ui/CoffeeExplorer/translations/Chinese.lang` | 1 language file |

The two changes are independent and can be taken separately.

---

## 1. Import directory: reorder modules with the mouse

### The problem

The import directory only offered **Move up** and **Move down**, which swap a descriptor with its neighbour. The load order directly affects how the PE loader walks the import table, but moving the last module to the first position meant clicking the arrow over and over again.

### How to use it

In the import directory list, **press and hold the left mouse button on any row and drag it**; release over the destination and it is moved there in one step.

| Action | Result |
|---|---|
| Release over the **upper half** of a row | Insert **before** that row |
| Release over the **lower half** of a row | Insert **after** that row |
| Release over the empty area **above** the rows | Move to the **first** position |
| Release over the empty area **below** the rows | Move to the **last** position |
| Release over the original spot, or outside the list | Nothing is modified |

Details:

- The drag threshold uses the system values (`SystemParameters.MinimumHorizontalDragDistance` / `MinimumVerticalDragDistance`), so it feels like Explorer and other native controls: **a slight wobble does not start a drag**, and cells can still be selected normally.
- After the drop the view is rebuilt, **the moved descriptor stays selected**, and the status bar reports "Import descriptor moved".
- The whole move is **a single** edit, so `Ctrl+Z` restores the original order once instead of N times.

### Safety boundaries

Dragging rewrites the order of the import descriptors in the file, so it is gated:

| Restriction | Reason |
|---|---|
| Only the **import directory main list** accepts drops | Only that list exposes the `imports.move_up` / `imports.move_down` commands; the section table and the per-module function list do not respond to dragging |
| Disabled while the table is **sorted** | After clicking a column header the visual order no longer matches the file order, so dragging by visual position would move the wrong descriptor; restore the natural order first |
| **Read-only files** cannot be dragged | There is no writable data |
| Fewer than 2 rows | Nothing to reorder |

### Implementation

**Core** — `core/src/Operations.cpp`, a new `imports.move` command handled in the same branch as `imports.move_up` / `imports.move_down`:

| Parameter | Meaning |
|---|---|
| `index` | Index of the descriptor being dragged |
| `target` | Destination index (where it ends up) |

Key points:

- Each `IMAGE_IMPORT_DESCRIPTOR` is 20 bytes; the base is obtained from `image.RvaToOffset(image.GetDirectory(DIR_IMPORT).m_Rva)`.
- The descriptor is copied to a temporary buffer, then `std::memmove` shifts the whole range in between by one slot (upward and downward drags handled separately), and the buffer is written back at `target`. Because the block is shifted rather than swapped repeatedly, crossing any number of entries costs one linear copy and produces a single undo record.
- Validation: `index < count`, `target < count`, `target != index`. Invalid arguments return `ERR_INVALID_ARGUMENT`; nothing is written out of bounds.
- The resulting `message` / `select` / `description` match the existing move commands, so the undo stack, view rebuild and status bar reuse the current machinery.
- **The existing `imports.move_up` / `imports.move_down` behaviour is unchanged**, keeping old entry points, shortcuts and scripts compatible.

**UI** — `ui/CoffeeExplorer/Controls/TableView.cs`, generic row dragging added to the table control:

- `ROW_FORMAT` (`"CoffeeExplorer.TableRow"`) is the drag data format; a `ReorderState` attached property on the `ListView` holds the candidate row, the press position and the drag state.
- `MouseDown` records the candidate row, `MouseMove` starts `DragDrop.DoDragDrop` once the system threshold is exceeded, `MouseUp` clears the state. `AllowDrop` is only enabled for lists that can reorder, so the default drag and drop behaviour of other lists is untouched.
- `DragOver` only reports the effect (`Move` when reordering is allowed, otherwise `None`). `Drop` computes the destination: over a row, the upper and lower halves of its height decide insert-before or insert-after; outside the rows, the upper and lower halves of the list decide first or last. It then corrects for the dragged row leaving its own slot (subsequent indexes shift up by one) and calls `imports.move`.
- `CanReorder()` is the single gate: the list exposes a move command, has at least 2 rows, is not read-only, and is not sorted. Making another list draggable only requires exposing the `imports.move_up` command; no control changes are needed.

---

## 2. Simplified Chinese language file

### Added

```
ui/CoffeeExplorer/translations/Chinese.lang
```

| Property | Value |
|---|---|
| Size | 68,503 bytes |
| Entries | 1,208 (identical set to `German.lang`) |
| Lines | 1,247 |
| Encoding | UTF-8 **with BOM**, CRLF line endings, no trailing newline (same as German and Russian) |
| Header | `@Name=简体中文`, `@Culture=zh-CN` |

### Enabling it

At startup the program scans `translations/*.lang`, shows `@Name` in the language list and uses `@Culture` as the UI culture. **No configuration change and no rebuild are required**:

- In the interface: **Tools → Options → Appearance → Language → 简体中文** (switches live and is written to `settings.json`);
- Or edit `settings.json` directly: `"Language": "Chinese"` (the value is the file name without its extension).

### Translation conventions

`German.lang` was used as the skeleton and translated entry by entry, with **identical keys in identical order** — a key with even a small typo silently falls back to English, which is the most common hidden defect in language files.

- All `{0}`–`{4}` placeholders were verified for count and order; `\=` and `\\` escape counts match (a missing one truncates the value of that line).
- Access keys follow the Chinese Windows convention: `文件(_F)`, `编辑(_E)`, keeping the original letter so the Alt shortcuts still work.
- Terminology: Section → 节, Offset → 偏移, Overlay → 覆盖数据, Ordinal → 序号, Directory → 目录.
- **18 entries are left in English**, all of them technical tokens or specification constants that should not be translated:
  `Detect It Easy`, `Detect It Easy (Ctrl+4)`, `RCData`, `VxD`, `HTML`, `Portable Executable 32 (PE32) DLL`, `Portable Executable 64 (PE32+) DLL`, `Name, Byte[8]`, `Size, Dword`, `AddressOfFunctions[ index ], Dword`, `UNWIND_INFO RVA, Dword`, `VA, Qword`, `VA, Dword`, `RVA, Dword`, `Age`, `ID`, `Windows GUI`, `Windows CE GUI`.
- In the resource language list, `Chinese (Taiwan)` is rendered as 中文（中国台湾） and `Chinese (Simplified)` as 中文（简体）.
- Structure field type annotations (`Byte[8]`, `Dword`, `Qword`) and flag constants (`CNT_CODE`, `MEM_EXECUTE`, …) are kept verbatim to match the specification.

> The *Languages* section of the upstream README still lists only English, German and Russian. A pull request to upstream would need one extra line there.

---

## Building

### Requirements

- Visual Studio 2022 (MSVC v143) + Windows SDK 10
- CMake ≥ 3.20, Ninja
- .NET 8 SDK (the interface is published framework-dependent, so the target machine needs the .NET 8 Desktop Runtime)

### Steps

```bat
:: 1) Build the C++ core, pecore.dll
cmake -S core -B out\core -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out\core --parallel

:: 2) Publish the WPF interface (PeCoreDir must end with a forward slash)
dotnet publish ui\CoffeeExplorer\CoffeeExplorer.csproj -c Release -r win-x64 ^
  --self-contained false "-p:PeCoreDir=<absolute repo path>/out/core/" -o out\bin
```

The output lands in `out\bin\`: `CoffeeExplorer.exe`, `CoffeeExplorer.dll`, `pecore.dll`, `translations\`, `themes\`, `Package\`. The `die\` folder from the release package (Detect It Easy) has to be copied in as well, otherwise *File info* is unavailable.

**Pitfalls verified here**

1. **The trailing slash on `PeCoreDir`**: the property is used to build `$(PeCoreDir)pecore.dll`. Without the trailing slash it becomes `corepecore.dll`, the build "succeeds" but silently skips copying `pecore.dll`, and the program will not start. A trailing backslash is swallowed by command line parsing and eats the following quote. **Forward slash plus trailing slash** is the only reliable form.
2. **Single instance**: `App.xaml.cs` uses a mutex plus a named pipe, so when an instance is already running a second launch hands the file over and exits — which looks like an instant crash but is the intended behaviour. Pass `--new-window` to open another window.
3. **`build.ps1` in a restricted environment**: the script sets `$ErrorActionPreference="Stop"`, which turns the CMake deprecation warning printed to stderr into a terminating error; unsigned scripts are also blocked by the execution policy (`Set-ExecutionPolicy -Scope Process Bypass`). When `reg.exe` is unavailable in a sandbox, CMake/MSVC detection fails; run the commands above step by step in that case, linking and embedding the manifest manually with `link.exe` and `mt.exe`.

### Verification

- The process stays alive, `MainWindowTitle = Coffee Explorer`, `Responding = True`, `settings.json` is persisted on exit, and there is no error in `CoffeeExplorer.log`.
- Chinese interface checked in practice: menus, structure tree, column headers, status bar and error messages are translated, with no entry falling back to English.

---

## Files changed

```
core/src/Operations.cpp                       # new imports.move (drag reorder); move_up/move_down untouched
ui/CoffeeExplorer/Controls/TableView.cs       # row drag reordering: ReorderState / CanReorder / DragOver / Drop
ui/CoffeeExplorer/translations/Chinese.lang   # new: Simplified Chinese (1,208 entries)
```

Apart from these three files the tree is identical to upstream v1.0.0.

---

## Notes for rebasing onto upstream

1. The `imports.*` command branch in `Operations.cpp` is the main conflict point. If upstream refactors it, only the arbitrary-position move behind `id == "imports.move"` needs to survive; the UI depends on the command name `imports.move` and its `index` / `target` parameters.
2. The drag code in `TableView.cs` is a **new block** spanning `ROW_FORMAT` through `OnReorderDrop`; keep it as a whole in a conflict. The only overlap with existing code is where `MouseDown` / `MouseMove` / `MouseUp` / `AllowDrop` are wired up.
3. `Chinese.lang` is a pure addition and cannot conflict, but if upstream adds or removes English keys the matching Chinese lines must be added or removed too — otherwise new strings simply appear in English, without any error.
