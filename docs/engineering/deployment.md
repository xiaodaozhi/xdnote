# 部署诊断与实际依赖清单

本阶段准备独立运行目录与自动核查工具，不创建安装器、不注册文件关联、不签名或发布。当前 Windows 诊断目录包含产品二进制及离屏探针；后者、Qt Test 和 offscreen 插件属于诊断用途，发行时须另行排除并重新验证。macOS／Linux 的部署目录和安装格式仍须在对应平台实施，不能用 Windows 清单代替。

## Windows 诊断入口

先使用默认 VS2022 配置构建，再显式提供本项目构建、Qt SDK、全新的目标目录及系统安装中的 x64 CRT 再分发目录：

```text
python scripts/deployment_inventory.py --build-dir build/desktop-admission --qt-prefix build/Qt/6.8.2/msvc2022_64 --destination build/cycle-delivery/staging --redist-dir "<已安装 VS2022>/VC/Redist/MSVC/<版本>/x64/Microsoft.VC143.CRT"
```

`<版本>` 须按实际安装填写；不存在的工具不会自动下载或跨项目回退。目标必须位于本项目 `build/` 内，已有目录一律拒绝覆盖。脚本核对构建来源，使用同一 SDK 的 `windeployqt` 分析 Qt／QML 依赖，保留软件图形回退库，补齐 WebEngine 资源与 locale，仅按实际导入复制显式指定的 CRT 文件。

正常导入和延迟导入通过只读 PE 解析核对，不加载被分析的 DLL。未找到的依赖、损坏导入表及链接目录导致失败。当前主机的 Windows 系统库与 API set 单独分类：主机存在某库不能证明最低 Windows 版本支持。

脚本随后清除继承的 Qt、QML 和 SDK 搜索路径，只允许目标目录及系统目录，使用离屏探针核对明暗预览。该步骤不启动产品窗口；它验证当前主机上 Widget／WebEngine 和插件的部署，不代表原生安装或完整应用验收。旁挂 `*-evidence/` 保存日志、截图和 `inventory.json`，后者记录每个实际文件的字节数、SHA-256、导入闭包、构建身份、诊断专用文件及未完成事项。

## 许可材料和限制

[Qt Windows 部署说明](https://doc.qt.io/qt-6.8/windows-deployment.html)和 [WebEngine 部署说明](https://doc.qt.io/qt-6.8/qtwebengine-deploying.html)说明运行依赖；[WebEngine 许可说明](https://doc.qt.io/qt-6.8/qtwebengine-licensing.html)区分 Qt 与 Chromium 第三方内容。实际版本以锁定的 Qt 6.8.2 SDK 和二进制为准，6.8 系列在线文档不能替代该版本的许可输入。

诊断目录带入项目 Apache-2.0、CC BY 4.0、原创设计署名、已核验的 Rust／Scintilla 许可文本，以及[固定 Qt 许可原文与摘要](../../tools/deployment-notices.json)。工具依据实际部署的二进制名称保留匹配的 Qt 模块 SPDX 文件，并输出其中版权及已嵌入许可原文。模块级 SBOM 可能包含构建工具或未使用组件，不把它作为准确的产品组件子集。

当前 Qt 6.8.2 SDK 的模块 SBOM 未列出完整 Chromium 组件与许可原文，且该构建没有 `chrome://credits/` 页面；不能从现有二进制自动提取完整声明。报告将此项明确记录为缺口，最终分发须补齐匹配的 QtWebEngine 源码及按实际构建配置生成的第三方声明；不能改用其他 Qt 版本的网页充数。Qt 模块 SBOM、Chromium 声明和项目许可分别核对，不将 Apache-2.0 标注为整个目录的统一许可证。

文件清单及文本齐备检查不自动构成发行许可结论。实际分发仍须核对对应 Qt／Chromium 源码提供及用户替换／重新链接等适用义务、MSVC 运行库的再分发条件、签名和安装方案，并在最终包内容变化后重新生成清单。当前不捆绑字体；新增字体或第三方素材时必须重新核对。最终交付与最低系统核验继续由 #4、#10、#11 跟踪。
