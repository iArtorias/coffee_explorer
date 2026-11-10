# Coffee Explorer — Fork 更新说明

> 语言 / Language: **简体中文** · [English](FORK_CHANGES.en.md)

本仓库基于上游 [iArtorias/coffee_explorer](https://github.com/iArtorias/coffee_explorer) **v1.0.0**，在保持原有架构与行为的前提下做了两处本地增强。

| 项目 | 内容 |
|---|---|
| 上游版本 | Coffee Explorer 1.0.0 |
| 本 fork 变更 | ① 导入目录支持鼠标拖拽排序　② 新增简体中文语言文件 |
| 变更日期 | 2026-11-10 |
| 许可证 | GNU GPL v3（与上游一致，本 fork 的修改同样以 GPLv3 发布） |

---

## 变更一览

| # | 变更 | 影响范围 | 新增文件 |
|:--:|---|---|---|
| 1 | 导入表条目拖拽排序 | `core/src/Operations.cpp`、`ui/CoffeeExplorer/Controls/TableView.cs` | 无 |
| 2 | 简体中文界面 | `ui/CoffeeExplorer/translations/Chinese.lang` | 1 个语言文件 |

两项改动互不依赖，可单独取用。

---

## 1. 导入目录：鼠标拖拽调整模块顺序

### 问题

「导入目录」原本只提供 **上移（Move up）** 和 **下移（Move down）** 两个相邻交换命令。加载顺序直接影响 PE 加载器的解析顺序，而把一个末尾的模块排到第一位，只能靠连续点击上移，条目多时非常低效。

### 使用方式

在导入目录列表中，**鼠标左键按住任意一行拖动**，拖到目标位置松开即可，一次到位。

| 操作 | 结果 |
|---|---|
| 拖到某行**上半部**松开 | 插入到该行**之前** |
| 拖到某行**下半部**松开 | 插入到该行**之后** |
| 拖到列表空白区域（上半） | 移到**第一位** |
| 拖到列表空白区域（下半） | 移到**最后一位** |
| 拖到原位置 / 列表外 | 不产生任何修改 |

细节：

- 移动阈值使用系统参数（`SystemParameters.MinimumHorizontalDragDistance` / `MinimumVerticalDragDistance`），与资源管理器等原生控件手感一致，**轻微抖动不会误触发拖拽**，仍可正常点选单元格。
- 松手后视图重建，**被移动的条目自动保持选中**，状态栏提示「导入描述符已移动」。
- 整个移动是**一次**编辑操作，`Ctrl+Z` 一次即可撤销回原位（不需要撤销 N 次）。

### 安全边界

拖拽会直接改写文件的导入描述符顺序，因此加了以下限制：

| 限制 | 原因 |
|---|---|
| 仅对**导入目录主列表**生效 | 只有该列表提供 `imports.move_up` / `imports.move_down` 命令；节表、模块详情（导入函数）等其他列表不响应拖拽 |
| 表格**处于排序状态时禁用** | 点表头排序后视觉顺序 ≠ 文件顺序，按视觉位置搬运会改错条目；需恢复原始排序后再拖 |
| **只读文件**不可拖 | 无可写数据 |
| 少于 2 行时不可拖 | 无排序意义 |

### 实现

**core 层** — `core/src/Operations.cpp`，新增 `imports.move` 命令，与 `imports.move_up` / `imports.move_down` 同分支处理：

| 参数 | 含义 |
|---|---|
| `index` | 被拖动的描述符下标 |
| `target` | 目标下标（移动后它所在的位置） |

实现要点：

- 每个 `IMAGE_IMPORT_DESCRIPTOR` 固定 20 字节，基址由 `image.RvaToOffset(image.GetDirectory(DIR_IMPORT).m_Rva)` 求得。
- 先把待移动的描述符拷到临时缓冲，再用 `std::memmove` 把中间区间整体平移一格（向上拖或向下拖分别处理），最后回写到 `target`。整体搬移而非逐次交换，因此无论跨越多少条，代价都是一次线性拷贝、一条撤销记录。
- 校验：`index < count`、`target < count`、`target != index`，非法参数返回 `ERR_INVALID_ARGUMENT`，不会越界写入。
- 完成后的 `message` / `select` / `description` 与原有移动命令保持一致，撤销栈、视图重建、状态栏提示沿用现有机制。
- **原有 `imports.move_up` / `imports.move_down` 行为未改动**，旧入口、快捷键、脚本兼容。

**UI 层** — `ui/CoffeeExplorer/Controls/TableView.cs`，为通用表格控件增加行拖拽支持：

- `ROW_FORMAT`（`"CoffeeExplorer.TableRow"`）作为拖拽数据格式，`ReorderState` 以附加属性挂在 `ListView` 上，记录起始行、按下坐标与拖拽状态。
- `MouseDown` 记录候选行，`MouseMove` 超过系统阈值后调用 `DragDrop.DoDragDrop`，`MouseUp` 清理状态；`AllowDrop` 仅在该列表可重排时才打开，避免干扰其他列表的默认拖放行为。
- `DragOver` 只做效果反馈（可移动则显示 `Move` 光标，否则 `None`）；`Drop` 计算落点：命中行按该行高度上下各半判定插前/插后，未命中则按列表高度上下半判定首/末位；随后修正「被拖行离位导致的下标左移」，再调用 `imports.move`。
- `CanReorder()` 是唯一的准入判断：列表有移动命令、行数 ≥ 2、非只读、未排序。新增可拖拽列表时只需让它暴露 `imports.move_up` 命令，无需再改控件。

---

## 2. 简体中文语言文件

### 新增

```
ui/CoffeeExplorer/translations/Chinese.lang
```

| 属性 | 值 |
|---|---|
| 大小 | 68,503 字节 |
| 条目 | 1,208 条（与 `German.lang` 完全一致） |
| 行数 | 1,247 行 |
| 编码 | UTF-8 **带 BOM**，CRLF 换行，末行不带换行符（与 German / Russian 一致） |
| 声明 | `@Name=简体中文`、`@Culture=zh-CN` |

### 启用方式

程序启动时扫描 `translations/*.lang`，读取 `@Name` 显示在语言列表中，`@Culture` 作为 UI 区域设置，**无需修改配置或重新编译**：

- 界面内：**选项 → 外观 → 语言 → 简体中文**（即时切换，写入 `settings.json`）；
- 或直接改 `settings.json`：`"Language": "Chinese"`（值即文件名去掉扩展名）。

### 翻译约定

以 `German.lang` 为骨架逐条对照，保证**键名逐条逐序 100% 相同**——键名一旦有偏差，该条会静默回退英文，是语言文件最常见的隐性缺陷。

- 全部 `{0}`–`{4}` 占位符的数量与顺序已校验一致；`\=`、`\\` 转义数量一致（缺失会导致该行值被截断）。
- 助记符按中文 Windows 惯例：`文件(_F)`、`编辑(_E)`，保留原字母对应 Alt 快捷键。
- 术语统一：Section → 节，Offset → 偏移，Overlay → 覆盖数据，Ordinal → 序号，Directory → 目录。
- 以下 **18 条保持英文原文**，均为不该翻译的技术记号或规格常量：
  `Detect It Easy`、`Detect It Easy (Ctrl+4)`、`RCData`、`VxD`、`HTML`、`Portable Executable 32 (PE32) DLL`、`Portable Executable 64 (PE32+) DLL`、`Name, Byte[8]`、`Size, Dword`、`AddressOfFunctions[ index ], Dword`、`UNWIND_INFO RVA, Dword`、`VA, Qword`、`VA, Dword`、`RVA, Dword`、`Age`、`ID`、`Windows GUI`、`Windows CE GUI`。
- 资源语言列表中的 `Chinese (Taiwan)` 译为「中文（中国台湾）」，`Chinese (Simplified)` 译为「中文（简体）」。
- 结构体字段的类型标注（`Byte[8]`、`Dword`、`Qword`）与标志常量（`CNT_CODE`、`MEM_EXECUTE` 等）保持原样，与规格文档一致。

> 上游 README 的 *Languages* 章节目前只列出 English / German / Russian，未包含本 fork 新增的中文；如要向上游提交 PR，需同步补一行。

---

## 构建

### 环境

- Visual Studio 2022（MSVC v143）+ Windows SDK 10
- CMake ≥ 3.20、Ninja
- .NET 8 SDK（界面为非自包含发布，目标机需安装 .NET 8 Desktop Runtime）

### 步骤

```bat
:: 1) 构建 C++ 核心 pecore.dll
cmake -S core -B out\core -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out\core --parallel

:: 2) 发布 WPF 界面（注意 PeCoreDir 必须以正斜杠结尾）
dotnet publish ui\CoffeeExplorer\CoffeeExplorer.csproj -c Release -r win-x64 ^
  --self-contained false "-p:PeCoreDir=<仓库绝对路径>/out/core/" -o out\bin
```

产物位于 `out\bin\`：`CoffeeExplorer.exe`、`CoffeeExplorer.dll`、`pecore.dll`、`translations\`、`themes\`、`Package\`。发行包中的 `die\` 目录（Detect It Easy）需一并拷入，否则「文件信息」不可用。

**已验证的坑**

1. **`PeCoreDir` 结尾斜杠**：该属性用于拼 `$(PeCoreDir)pecore.dll`。不带结尾斜杠会拼成 `corepecore.dll`，构建"成功"但静默漏拷 `pecore.dll`，运行时直接起不来；用反斜杠结尾又会被命令行解析吃掉后续引号。**正斜杠 + 结尾斜杠**是唯一稳妥写法。
2. **单实例**：`App.xaml.cs` 用 Mutex + 命名管道实现单实例，检测到已有实例时会把文件移交过去并自行退出——看起来像"启动即闪退"，实为正常行为。需要多窗口请加 `--new-window`。
3. **`build.ps1` 在受限环境下的问题**：脚本内 `$ErrorActionPreference="Stop"` 会把 CMake 输出到 stderr 的 deprecation warning 当成终止错误；未签名脚本还受执行策略限制（`Set-ExecutionPolicy -Scope Process Bypass`）。沙箱中 `reg.exe` 不可用时 CMake/MSVC 探测会失败，此时可按上面的命令分步执行，并用 `link.exe` + `mt.exe` 手动链接与补嵌 manifest。

### 验证记录

- 进程存活、`MainWindowTitle = Coffee Explorer`、`Responding = True`，退出后 `settings.json` 正常持久化，无 `CoffeeExplorer.log` 错误日志。
- 中文界面实测：菜单、结构树、列头、状态栏、错误消息均为中文，未出现回退英文的条目。

---

## 改动文件清单

```
core/src/Operations.cpp                       # 新增 imports.move（拖拽移动），原 move_up/move_down 不变
ui/CoffeeExplorer/Controls/TableView.cs       # 表格行拖拽重排：ReorderState / CanReorder / DragOver / Drop
ui/CoffeeExplorer/translations/Chinese.lang   # 新增：简体中文（1208 条）
```

除上述三项外，仓库其余文件与上游 v1.0.0 一致。

---

## 与上游同步时的注意点

1. `Operations.cpp` 的 `imports.*` 命令分支是本 fork 的主要冲突点；上游若重构该分支，只需保留 `id == "imports.move"` 的任意位置搬运逻辑即可，UI 侧依赖的是命令名 `imports.move` 与 `index` / `target` 两个参数。
2. `TableView.cs` 的拖拽代码是**新增区块**，集中在 `ROW_FORMAT` 常量至 `OnReorderDrop` 之间，冲突时整块保留即可；唯一与既有代码交叠的是 `MouseDown` / `MouseMove` / `MouseUp` / `AllowDrop` 的挂接位置。
3. `Chinese.lang` 为纯新增文件，无冲突风险；但上游若增删了英文词条，需要同步增删对应中文行，否则新词条会显示英文（不报错）。
