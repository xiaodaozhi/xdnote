# 实际分发文件与许可证据

`scripts/distribution_audit.py` 对 Windows 精简产品载荷逐文件核对材料，输入为已封存的文件清单和同一项目 Qt SDK。它检查文件大小／SHA-256、必需许可与署名、原始安装引擎材料及 Microsoft VC++ 运行库协议；遗漏、修改、未分类文件和不一致的 SDK 文件会使检查失败。输出仍保留 `licenseReviewComplete=false` 与 `releaseReady=false`，材料核对不等于最终许可结论。

```text
python scripts/distribution_audit.py --root <产品载荷目录> --audit <产品清单.json> --qt-prefix <本项目 Qt 6.8.2 目录> --report <build 下的新报告.json>
```

Qt DLL 和插件按照 SDK SPDX 中的相对路径对应到实际分发文件，并另行核对实际 SDK 文件的 SHA-256。供应方 SPDX 原文、许可选项、版权、文件校验值及模块级未确定字段原样记录。SDK 文件一致与 SPDX 文件校验值一致分别报告：上游校验值不同不能直接认定文件遭到篡改，也不能未经证据归因为签名或重定位。差异须单列跟进。

对于带尾部证书表的 PE64 文件，报告另做只读比较：去掉证书表、还原校验和与安全目录字段，检查是否与供应方原始 SHA-1 一致。原始校验值和实际 SDK／载荷摘要保持不变；可解释的证书相关字节变化与仍未解释的差异分开记录。这项比较不验证签名信任链，也不作许可结论。

固定 Qt Network 派生构建使用 `source-access.json.networkBackports` 绑定源码、补丁及原始／实际 SDK 摘要。Windows 先核对保留的原始 DLL 与供应方 SPDX 校验值，再核对新 SDK 和载荷；Unix 保留新模块的派生记录与部署代码节比较。原始上游校验值保持原文，派生差异有独立来源，不合并为未解释的变化；许可与发行完成状态仍为 false。

模块级 SPDX 可能包含构建工具与未分发组件。精简产品元数据的 `matchedBinaryNames` 只列实际文件，移除的 Qt Test、offscreen 不再出现在其中；上游完整模块的包条目和未确定许可字段不据此删改，不把它们误报成产品实际引入的全部组件。

应用主程序单列自有代码与静态链接的锁定 Rust 包、Scintilla 材料；Rust 标准库原始版权清单及运行时许可按固定编译器提交与文本摘要收集，包级材料不直接代表精确静态链接范围。实际编入的 Rust／运行时组件、Qt 对应源码与用户替换等适用要求和最终交付绑定仍需核验，方法见[开发交付](developer-delivery.md)。Microsoft VC++ 协议仅用于其覆盖的运行库，不推定它覆盖其他 Microsoft 文件。

Windows 最低目标为 Windows 10 1809+。`D3Dcompiler_47.dll` 使用目标系统提供的版本，依据[微软组件说明](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/packaging/packaging-framework-packages?view=gdk-2604)；部署工具使用 `--no-system-d3d-compiler`、`--no-system-dxc-compiler`，不将宿主 Windows 的图形 DLL 复制进载荷。图形 API 继续由目标系统提供，静态导入与实际运行仍分别核对；最低系统真机验收单独记录。

报告绑定实际文件摘要和构建身份，单次结果记录在 Issue／PR 或构建工件中。完整许可、源码／替换材料与最终交付核验仍由 [Issue #4](https://github.com/xiaodaozhi/xdnote/issues/4) 跟踪，采用[既定材料归属与隔离替换方法](release-and-updates.md)。已识别且适用的义务或必需材料未解决时阻断候选；上游 `NOASSERTION` 或机器报告不完整本身不直接推导为许可冲突，也不自动作为豁免。签名／公证采用与否按发行决策记录，不与许可核验混为同一门槛。

## Qt 源码与发行附件边界

xdnote 的 GitHub Release 附件、安装包、应用压缩包及便携产品目录均不包含 Qt 源码归档或源码树。应用继续携带实际需要的 Qt 运行库、许可文本、版权与第三方声明；发行清单不得把 Qt 源码作为本项目 Release 的额外附件。

Qt 对应源码获取和用户修改／替换库的适用要求另行通过独立方式落实，所选方式须与实际 Qt 版本、修改和分发条件相符，并核对获取渠道与必要说明的可用性。该项继续由 Issue #4 跟踪；记录链接或一份说明不直接视为全部要求已完成，也不因此移除当前 `licenseReviewComplete=false`／`releaseReady=false` 状态。依据见 [Qt LGPL 使用条件](https://www.qt.io/development/open-source-lgpl-obligations)。

Windows 产品安装专项和 Unix 产品部署专项的诊断进程使用固定提交的 Noto Sans CJK SC 2.004 字体夹具，避免服务器缺少中文字体时跳过字形检查。`scripts/ci-font-materials.json` 锁定字体和原始 OFL 文本摘要，`prepare_ci_font.py` 准备到本项目 build；仅诊断入口通过应用级字体 API 显式载入，不安装到操作系统，不进入产品载荷。字体夹具结果不代替实际目标系统字体与可见桌面验收。

Linux／macOS 的 `product_deployment_unix.py` 另保存纯产品逐文件材料：实际内容 SHA-256、内部相对链接目标、Qt SDK 路径和原始摘要、准确 SPDX 文件记录，以及目录内加载路径修正／ad-hoc 重新封印后的可执行节比较。`unix_binary_evidence.py` 只读解析 ELF64、Mach-O 及通用 Mach-O 的代码节；ICU 这类纯数据 ELF 改为核对只读数据节，截断文件、没有可核对内容或节内容变化会使检查失败。数据、加载命令和签名的变化另行记录，不将代码节核对当作完整文件等同或签名验证。全部文件必须分类，上游模块未确定字段仍保留；最终源码、替换、运行库与正式发行核验仍保持待办。

Linux SDK 附带的 ICU 73.2 三份库以 `tools/unix-runtime-notices.json` 单列来源和原始许可摘要，只有实际出现对应 SDK 文件时才随包收集。完整许可取自 ICU 官方固定提交，包含其第三方声明；不把 Qt 模块 SPDX 缺少精确 ICU 文件条目写成文件已被覆盖，也不把数据文件无指令节误判为损坏。ICU 仍按实际 SDK 字节、重定位后的节摘要和独立许可材料核对，最终编入范围及发行核验继续保留。

桌面主程序生成 `xdnote-link.map`，可通过 `scripts/linkage_audit.py --build-dir <当前构建目录> --output <build 下的报告.json>` 将被链接器选择的 Rust 服务归档成员关联到固定 Cargo 材料或编译器自带 rlib 身份。原始 map 摘要、构建身份和成员列表共同保留；成员被选入不表示其中每一行、内嵌库或代码都未被优化移除，仍须复核对应组件范围。该报告不自动将完整许可或发行状态改为通过。

MSVC map 的 `library:member` 可省略 `.lib` 后缀；其中 DLL 导入描述符单独分类，表示链接器选择的导入名称，不将外部 DLL 的内容或许可归属推定为已核对。

macOS Sparkle 实际 framework／helper 的供应方文件、arm64 指令节及原始许可和构建回执纳入纯产品部署证据；重建签名数据与 SDK 字节一致分别记录。Qt 的隔离源码修改／替换探针通过实际加载路径及仅修改库导出的证明函数核对来源，并继续验证阅读、图片、编辑、撤销、保存及同会话接管；该工程验证不改变固定 Qt SDK，也不替代全部对应源码提供条件的最终核验。

源码 Core 构建与替换入口覆盖 Windows／Linux／macOS，方法见[自行替换说明](qt-library-replacement.md)。PE／ELF／Mach-O 读取实际指令节，运行报告保留实际 Core 加载路径、证明函数、SDK 与输入摘要及维护拒绝；三平台的最终结论须以各次原始报告为准，不从某个平台通过推定其他平台或全部 Qt 模块通过。

`scripts/component_review.py` 从同一实际产品的材料清单和 linker 报告出发，核对原始 Qt SPDX 摘要及 SDK 文件，定位实际分发文件所属包，再追溯供应方依赖／链接关系。编译器的构建依赖、整个模块的包含关系不作为追溯根；未被这些关系到达不等于二进制中不存在。包级许可选项和未确定字段保持原文，跨文档引用必须匹配供应方声明的摘要，否则保留为待核验引用，不仅凭同名包修复。

手动 `delivery-review.yml`（或桌面入口 `scope=delivery-review`）要求显式提供有未过期实际产品工件的 Qt 替换运行 ID。工具只读获取同仓库成功运行、核对 ZIP 摘要和报告身份，在三平台固定 SDK 上生成审阅清单；运行中的工具提交与来源产品提交分别记录。不构建、签名、安装、注册或发布软件，不读取生产私钥；原始材料审阅和源码／替换义务继续保持 `licenseReviewComplete=false`、`releaseReady=false`。已过期工件需重新生成源证据，不能拼接其他提交冒充同一候选。

跨文档引用可能指向 SDK 原始 tag-value `.spdx`，其摘要与 JSON 序列化不同。工具先核对被引用原文摘要，并严格比较两种序列化中用于追溯的文档身份、包许可／版权／版本、文件身份、去重关系和外部引用；仅这些字段一致才关联同一目标。未通过比较、摘要不符或缺失的目标继续保留缺口，不修改供应方原文，也不据此宣称全部 SPDX 字段等同。依赖关系采用供应方声明，通用 `DEPENDS_ON` 可同时涉及构建和运行，不代表已经证实最终机器码中的组件；依据见 [SPDX 文档引用](https://spdx.github.io/spdx-spec/v2.3.1/document-creation-information/)及[关系定义](https://spdx.github.io/spdx-spec/v2.3.1/relationships-between-SPDX-elements/)。
