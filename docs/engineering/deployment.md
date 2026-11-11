# 部署诊断与实际依赖清单

本入口生成 Windows 独立诊断目录，核对实际 DLL／插件闭包及随附许可输入。它不创建安装器、不签名或发布、不注册文件关联。目录包含诊断探针、Qt Test 和 offscreen 插件，不能直接作为发行包；macOS／Linux 部署仍须另行实现与验证。

## Windows 诊断入口

先使用默认 VS2022 配置构建，提供同一项目的构建、Qt SDK、全新目标目录及系统 VS2022 安装中的 x64 CRT 再分发目录：

```text
python scripts/deployment_inventory.py --build-dir build/desktop-admission --qt-prefix build/Qt/6.8.2/msvc2022_64 --destination build/cycle-delivery/staging --redist-dir "<已安装 VS2022>/VC/Redist/MSVC/<版本>/x64/Microsoft.VC143.CRT"
```

`<版本>` 按实际安装填写。目标必须在本项目 `build/` 内，已有目标或证据目录拒绝覆盖，不跨项目寻找缺失依赖。工具核对构建身份，使用同一 SDK 的 `windeployqt` 分析 Qt／QML 和插件，保留软件图形回退库，按实际导入复制显式指定的 CRT 文件。无 NMEA／GPS 功能，不部署可选 `qtposition_nmea` 插件；任何浏览器运行文件均导致失败。

正常与延迟导入采用只读 PE 解析，不加载被分析 DLL。未找到的依赖、损坏导入表、路径链接均导致失败。宿主系统库和 Windows API set 单独分类；宿主能解析不能证明最低 Windows 版本可运行。

随后清除继承的 Qt／QML 和 SDK 搜索路径，仅使用诊断目录与系统目录运行明暗原生预览探针，以及通过真实文件／会话入口打开自有临时样本的独立窗口探针。全部固定离屏，不操作真实桌面。旁边的 `<目标名>-evidence/` 保存日志、截图和 `inventory.json`，包含文件大小与 SHA-256、导入闭包、构建／工具身份、Qt 模块证据、诊断专用内容及剩余事项。

## 许可材料与发行边界

目录携带项目 Apache-2.0、CC BY 4.0、设计署名及已核验的 Rust／Scintilla 文本。[固定 Qt 6.8.2 文本清单](../../tools/deployment-notices.json)保留上游 LGPL-3.0 和其引用的 GPL-3.0 原文与摘要；保留 GPL 文本不表示将自有代码改为 GPL。文本与版本由日常仓库检查核对。

工具仅保留与实际 Qt 二进制匹配的 SDK 模块 SPDX 文件，汇总其中的版权、声明和已嵌入许可文本；已部署的 Qt 库缺少模块记录时失败。模块 SBOM 可能含未用组件和构建工具，`NOASSERTION` 字段在报告中保留，不能把模块记录当作准确产品组件子集或完整许可结论。

按 [Qt 许可说明](https://doc.qt.io/qt-6.8/licensing.html)、[固定版本 LGPL 原文](../../LICENSES/third-party/Qt-LGPL-3.0-only.txt)与实际分发内容，仍须完成适用的源码提供、用户替换／重新链接及第三方声明核验；仅复制动态库和文本不表示这些要求已经完成。MSVC 运行库另按[官方再分发说明](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files)及 [VS2022 可分发清单](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution)核对。本入口不捆绑字体；新增字体或素材须重新检查。

报告始终保留 `releaseReady=false` 与 `licenseReviewComplete=false`。最低系统、原生安装／升级／卸载、文件关联、签名、其他平台交付和真实桌面验收继续独立记录，不能由当前宿主的离屏诊断替代。部署方法参考 [Qt Windows 部署说明](https://doc.qt.io/qt-6.8/windows-deployment.html)，实际版本以锁定 SDK 和文件清单为准。
