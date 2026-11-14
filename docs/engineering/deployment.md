# 部署诊断与实际依赖清单

本入口生成独立诊断目录，核对实际动态库／插件闭包及随附许可输入。它不创建安装器、不进行发布者身份签名或发布、不注册文件关联。目录包含诊断探针、Qt Test 和 offscreen 插件，不能直接作为发行包；各平台实际通过情况以对应 Issue／PR 和工件为准。

## Linux／macOS 纯产品入口

`scripts/product_deployment_unix.py` 从实际应用和资源助手重新收集产品依赖，不从含测试程序的诊断目录删减猜测。Linux 保留所需 xcb／Wayland、图形集成、输入法、图片、TLS 与系统主题插件；macOS 不扫描准入 QML，仅部署产品 `.app` 的 framework／插件，保留内部相对链接、目录内加载路径和 ad-hoc 完整性封印。Quick／QML、主题、调试工具、Qt Test、测试程序或 offscreen 插件进入产品目录时失败。此处禁止的主题指 QML 主题，原生系统样式仍按实际依赖保留。

```text
python scripts/product_deployment_unix.py --build-dir <成功构建目录> --qt-prefix <本项目 Qt 6.8.2 目录> --destination <build 下的新产品目录>
```

封存前检查真实传递依赖、原始许可和实际文件；macOS 许可放在 `.app/Contents/Resources/licenses` 中，Linux 放在产品根目录 `licenses`。旁边的 evidence 目录保存清单、逐文件材料、明暗独立窗口与工作区截图及运行报告。无 Qt Test 依赖的外部部署探针和 offscreen 插件只存在于临时诊断目录，探针使用产品内的 Qt 与资源助手，验证自建图片、原文编辑／撤销／保存以及唯一会话接管，并核对实际加载位置和运行前后的产品摘要。临时探针退出后删除，不进入产品或证据工件。

Qt 库经过加载路径修正和 macOS 重新封印，最终摘要可能与 SDK 不同。逐文件保留 SDK 原始摘要、SPDX 文件声明、交付摘要及两者的可执行节摘要对照（ICU 纯数据 ELF 核对只读数据节），不把代码节一致扩展为整个文件一致或签名信任链结论。完整发行许可仍按[分发证据](distribution-audit.md)核验。

同一入口提供 `--payload <已封存产品目录> --audit <evidence/inventory.json> --destination <build 下的新安装验证目录>`，执行安装／升级后的明暗探针、修改文件和未知文件拒绝维护、卸载及自建数据保留。只操作专用目录，系统注册关闭；含未知文件时保持原安装不变，测试只移除自己创建的未知样本后继续卸载。

## Windows 诊断入口

先使用默认 VS2022 配置构建，提供同一项目的构建、Qt SDK、全新目标目录及系统 VS2022 安装中的 x64 CRT 再分发目录：

```text
python scripts/deployment_inventory.py --build-dir build/desktop-admission --qt-prefix build/Qt/6.8.2/msvc2022_64 --destination build/cycle-delivery/staging --redist-dir "<已安装 VS2022>/VC/Redist/MSVC/<版本>/x64/Microsoft.VC143.CRT"
```

`<版本>` 按实际安装填写。目标必须在本项目 `build/` 内，已有目标或证据目录拒绝覆盖，不跨项目寻找缺失依赖。工具核对构建身份，使用同一 SDK 的 `windeployqt` 分析实际产品与诊断入口的 Qt 导入和插件，按实际导入复制显式指定的 CRT 文件。Windows 10+ 已提供 D3DCompiler，明确禁止复制宿主系统 D3D／DXC 文件，详见[实际分发证据](distribution-audit.md)。无 NMEA／GPS 功能，不部署可选 `qtposition_nmea` 插件；任何浏览器运行文件均导致失败。产品不使用 Quick／QML，Windows 诊断入口不扫描准入窗口的 QML 目录；若载荷出现 Qt Quick、QML 模块、主题或 QML 调试工具则拒绝。`scripts/check_product_linkage.py` 另外只读检查产品二进制的直接导入，Unix 定向部署也执行此项，随后分别核对实际传递依赖。

正常与延迟导入采用只读 PE 解析，不加载被分析 DLL。未找到的依赖、损坏导入表、路径链接均导致失败。宿主系统库和 Windows API set 单独分类；宿主能解析不能证明最低 Windows 版本可运行。

随后清除继承的 Qt／QML 和 SDK 搜索路径，仅使用诊断目录与系统目录运行明暗原生预览探针，以及通过真实文件／会话入口打开自有临时样本的独立窗口探针。全部固定离屏，不操作真实桌面。旁边的 `<目标名>-evidence/` 保存日志、截图和 `inventory.json`，包含文件大小与 SHA-256、导入闭包、构建／工具身份、Qt 模块证据、诊断专用内容及剩余事项。

## Linux／macOS 诊断入口

在对应宿主使用同一项目的成功构建与 Qt 6.8.2 SDK，提供尚不存在的目标及证据目录：

```text
python scripts/deployment_unix.py --build-dir build/desktop-admission --qt-prefix build/Qt/6.8.2/gcc_64 --destination build/cycle-delivery/staging
```

上例为 Linux；macOS 的 SDK 子目录使用 `macos`。Linux 需要系统 `readelf`、`patchelf` 和 `ldconfig`；macOS 使用系统 `otool`、`install_name_tool`、`codesign` 与该 SDK 的 `macdeployqt`。源码／构建身份、SDK 归属和已有目录守卫与 Windows 入口一致。

Linux 的 SDK 准备显式包含独立 `qtwayland` 归档；按 ELF 的 `NEEDED` 递归复制当前 SDK 动态库，保留版本文件及相对 SONAME 链接，将每份已部署 ELF 的搜索路径改为目录内 `$ORIGIN`。候选包含 offscreen、xcb、Wayland 及其图形集成插件；宿主系统库单独记录，不复制 glibc 或声称无需系统依赖。依赖核对读取动态表，不执行 `ldd`。未解析依赖、外部库路径或 SDK／构建搜索路径均导致失败。

Unix 诊断目录仍保留含 Quick 准入用例的测试程序及其依赖，不能据整个诊断目录的模块清单判断产品本身依赖；产品直接导入检查单列。macOS 创建诊断 `xdnote.app`，产品、两个探针、资源助手及资源定向测试程序均进入 `Contents/MacOS`；`macdeployqt` 部署 frameworks／插件与 QML 输入，随后清除构建／SDK 搜索路径，核对每份 Mach-O 导入。framework 链接只允许指向诊断目录内。为允许重定位后的 ARM64 二进制执行，使用本机 ad-hoc 完整性封印；它不含发布者身份、不公证、不生成 DMG，也不是发行签名。系统 dyld 共享缓存库按系统依赖记录，不能用磁盘单文件缺失来判为丢库。

两平台均清除继承的 Qt／QML、动态库预加载与 SDK 路径，只使用诊断目录内的库／插件及系统目录运行自有样本：明暗阅读、真实临时 Markdown 的文件／会话入口，以及经资源助手读取的自生成图片。固定离屏，不激活用户窗口。旁边的 `<目标名>-evidence/` 保留日志、三张截图及 `inventory.json`；失败也写入错误与已完成元数据。清单记录实际文件摘要、内部链接、依赖定位、宿主、源码／构建身份、工具摘要和许可输入。宿主仍可能有开发环境，静态路径闭包和隔离搜索验证不等同于干净系统安装验收。

产品没有 Qt SQL 数据库功能。macOS 部署工具扫描带入的 `sqldrivers` 不进入诊断候选，避免无关数据库客户端成为隐含依赖；排除原因写入报告，不以开发机器上另行安装数据库库来掩盖外部依赖。

手动选择桌面工作流的 `scope=deployment-unix`，仅在 Linux x86_64、macOS Intel／ARM 执行上文纯产品部署与安装验证；`scope=deployment-linux` 只补验 Linux 专属变动。旧诊断入口保留给明确需要测试依赖的研发场景。专项不运行完整桌面测试矩阵，仅上传证据，不上传安装包或发布产品。

## 许可材料与发行边界

目录携带项目 Apache-2.0、CC BY 4.0、设计署名及已核验的 Rust／Scintilla 文本。[固定 Qt 6.8.2 文本清单](../../tools/deployment-notices.json)保留上游 LGPL-3.0 和其引用的 GPL-3.0 原文与摘要；保留 GPL 文本不表示将自有代码改为 GPL。文本与版本由日常仓库检查核对。

工具仅保留与实际 Qt 二进制匹配的 SDK 模块 SPDX 文件，汇总其中的版权、声明和已嵌入许可文本；已部署的 Qt 库缺少模块记录时失败。模块 SBOM 可能含未用组件和构建工具，`NOASSERTION` 字段在报告中保留，不能把模块记录当作准确产品组件子集或完整许可结论。

按 [Qt 许可说明](https://doc.qt.io/qt-6.8/licensing.html)、[固定版本 LGPL 原文](../../LICENSES/third-party/Qt-LGPL-3.0-only.txt)与实际分发内容，仍须完成适用的源码提供、用户替换／重新链接及第三方声明核验；仅复制动态库和文本不表示这些要求已经完成。MSVC 运行库另按[官方再分发说明](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files)及 [VS2022 可分发清单](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution)核对。本入口不捆绑字体；新增字体或素材须重新检查。

报告始终保留 `releaseReady=false` 与 `licenseReviewComplete=false`。最低系统、原生安装／升级／卸载、文件关联、发布者签名和真实桌面验收继续独立记录，不能由当前宿主的离屏诊断替代。部署方法参考 [Qt Windows](https://doc.qt.io/qt-6.8/windows-deployment.html)、[Linux](https://doc.qt.io/qt-6.8/linux-deployment.html)及 [macOS 部署说明](https://doc.qt.io/qt-6.8/macos-deployment.html)，实际版本以锁定 SDK 和文件清单为准。
