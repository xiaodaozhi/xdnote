# 实际分发文件与许可证据

`scripts/distribution_audit.py` 对 Windows 精简产品载荷逐文件核对材料，输入为已封存的文件清单和同一项目 Qt SDK。它检查文件大小／SHA-256、必需许可与署名、原始安装引擎材料及 Microsoft VC++ 运行库协议；遗漏、修改、未分类文件和不一致的 SDK 文件会使检查失败。输出仍保留 `licenseReviewComplete=false` 与 `releaseReady=false`，材料核对不等于最终许可结论。

```text
python scripts/distribution_audit.py --root <产品载荷目录> --audit <产品清单.json> --qt-prefix <本项目 Qt 6.8.2 目录> --report <build 下的新报告.json>
```

Qt DLL 和插件按照 SDK SPDX 中的相对路径对应到实际分发文件，并另行核对实际 SDK 文件的 SHA-256。供应方 SPDX 原文、许可选项、版权、文件校验值及模块级未确定字段原样记录。SDK 文件一致与 SPDX 文件校验值一致分别报告：上游校验值不同不能直接认定文件遭到篡改，也不能未经证据归因为签名或重定位。差异须单列跟进。

对于带尾部证书表的 PE64 文件，报告另做只读比较：去掉证书表、还原校验和与安全目录字段，检查是否与供应方原始 SHA-1 一致。原始校验值和实际 SDK／载荷摘要保持不变；可解释的证书相关字节变化与仍未解释的差异分开记录。这项比较不验证签名信任链，也不作许可结论。

模块级 SPDX 可能包含构建工具与未分发组件。精简产品元数据的 `matchedBinaryNames` 只列实际文件，移除的 Qt Test、offscreen 不再出现在其中；上游完整模块的包条目和未确定许可字段不据此删改，不把它们误报成产品实际引入的全部组件。

应用主程序单列自有代码与静态链接的锁定 Rust 包、Scintilla 材料；Rust 标准库及编译器运行时、Qt 对应源码与用户替换等适用要求、实际编入的第三方组件和最终交付绑定仍需核验。Microsoft VC++ 协议仅用于其覆盖的运行库，不推定它覆盖其他 Microsoft 文件。

Windows 最低目标为 Windows 10 1809+。`D3Dcompiler_47.dll` 使用目标系统提供的版本，依据[微软组件说明](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/packaging/packaging-framework-packages?view=gdk-2604)；部署工具使用 `--no-system-d3d-compiler`、`--no-system-dxc-compiler`，不将宿主 Windows 的图形 DLL 复制进载荷。图形 API 继续由目标系统提供，静态导入与实际运行仍分别核对；最低系统真机验收单独记录。

报告绑定实际文件摘要和构建身份，单次结果记录在 Issue／PR 或构建工件中。完整许可、源码／替换材料与最终交付核验仍由 [Issue #4](https://github.com/xiaodaozhi/xdnote/issues/4) 跟踪。

Windows 产品安装专项和 Unix 产品部署专项的诊断进程使用固定提交的 Noto Sans CJK SC 2.004 字体夹具，避免服务器缺少中文字体时跳过字形检查。`scripts/ci-font-materials.json` 锁定字体和原始 OFL 文本摘要，`prepare_ci_font.py` 准备到本项目 build；仅诊断入口通过应用级字体 API 显式载入，不安装到操作系统，不进入产品载荷。字体夹具结果不代替实际目标系统字体与可见桌面验收。

Linux／macOS 的 `product_deployment_unix.py` 另保存纯产品逐文件材料：实际内容 SHA-256、内部相对链接目标、Qt SDK 路径和原始摘要、准确 SPDX 文件记录，以及目录内加载路径修正／ad-hoc 重新封印后的可执行节比较。`unix_binary_evidence.py` 只读解析 ELF64、Mach-O 及通用 Mach-O 的代码节；截断文件、没有代码节或代码节变化会使检查失败。数据、加载命令和签名的变化另行记录，不将代码节核对当作完整文件等同或签名验证。全部文件必须分类，上游模块未确定字段仍保留；最终源码、替换、运行库与正式发行核验仍保持待办。
