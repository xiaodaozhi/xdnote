# 文档导航

本目录保存产品设计、视觉规范、架构决策依据、构建方法与通用验收方法。功能完成度以对应实现和实际验收为准，规格与参考图不是应用已完成的证明。

| 领域 | 文档 | 用途 |
| --- | --- | --- |
| 产品 | [需求](product/requirements.md) | 用户场景、需求来源、首版与后续范围 |
| 交互 | [交互规格](product/interaction-spec.md) | 打开路由、文档状态、窗口迁移、保存和关闭 |
| 工作区资源 | [图片与文档链接](product/workspace-resources.md) | 集中图片目录、相对链接、引用索引、移动维护与授权 |
| 控件 | [控件交互与焦点](product/control-interactions.md) | 点击反馈、Tab 外框、激活／取消后的清除与逻辑焦点 |
| 设置 | [设置规格](product/settings.md) | 主题与六种强调色、默认值、作用域和持久化 |
| 视觉 | [设计系统](visual/design-system.md) | 信息层级、跨界面公共组件一致性、缩放与材料权威 |
| 参数 | [tokens.json](visual/tokens.json) | 精确颜色、逻辑尺寸和字体参数 |
| 窗口 | [window-registry.json](visual/window-registry.json) | 窗口身份、建议尺寸、原稿与待补状态 |
| 工程 | [技术架构与实施基线](engineering/architecture.md) | 已确认取舍、模块契约、关键时序及待验证准入 |
| 预览选型 | [非 Chromium 预览方案评估](engineering/preview-alternatives.md) | 约束、候选、成本边界与后续准入 |
| 原生预览 | [全局语义模型](engineering/native-preview-model.md)、[阅读控件准入](engineering/native-preview-view.md) | 只读块树、布局／交互、资源和版本边界及未完成项 |
| 构建 | [桌面工程与本地构建](engineering/developer-build.md) | 固定依赖、分阶段入口、内存样本与实际准入边界 |
| 后台验证 | [无交互桌面自动化](qa/background-validation.md) | 固定离屏回归、自有样本性能、证据与真实桌面边界 |
| 部署 | [实际依赖与诊断目录](engineering/deployment.md) | Windows 隔离运行、DLL 闭包、Qt 模块许可输入与发行剩余项 |
| 文件基础 | [文件读取与会话登记](engineering/file-read-baseline.md) | 原生身份、严格字节基线、请求失效与当前限制 |
| 文件闭环 | [真实文件保存与冲突](engineering/file-save-workflow.md) | 冻结快照、替换保存、硬链接保护、冲突和关闭 |
| 独立窗口 | [视觉、交互与自动验证](engineering/standalone-window.md) | 共享控件、主题、阅读位置、反馈与验证边界 |
| 工作区基础 | [导航、标签与会话接管](engineering/workspace-foundation.md) | 显式目录、唯一会话、视图、关闭和自动化边界 |
| 资源基础 | [受控读取与引用分析](engineering/workspace-resource-foundation.md) | 路径、引用快照、实际对象授权与当前平台限制 |
| 窗口几何 | [跨显示器缩放与窗口几何](engineering/window-geometry.md) | 逻辑客户区、原生 DPI 约束、可用区、窗口状态与恢复 |
| 协作 | [产研与 GitHub 流程](engineering/workflow.md) | Issue、分支、PR、CI、验收及清理规则 |
| 许可 | [许可范围与依赖](engineering/licensing.md) | Apache-2.0、CC BY 4.0 的内容范围、署名与第三方依赖 |
| 平台 | [平台目标与验证基线](engineering/platform-support.md) | 最低系统、64 位架构、Linux 图形环境及实际验收边界 |
| 计划 | [开发路线](planning/roadmap.md) | 阶段顺序、产物和退出条件 |
| 验收 | [验收规格](qa/acceptance.md) | 需求映射、真实文件与平台验证方法 |
| 工作区界面 | [工作区界面核对](qa/workspace-visual.md) | 标签边界、侧栏、面包屑及导航页的明暗状态检查 |
| 覆盖 | [界面清单](visual/screen-inventory.md) | 33 张图板、89 个页面与状态 |
| 素材 | [交付说明](visual/asset-handoff.md)、[素材几何](visual/asset-geometry.json) | 原图切片、SVG、透明 PNG 与使用约定 |
| 原稿 | [设计材料索引](../design/README.md)、[静态画廊](../design/index.html) | 当前图组、品牌候选、精确样张与提示词 |

## 文档权威

1. 用户已明确提出的需求优先，需求文档区分已确认方向、设计推导和待决策项。
2. `product/interaction-spec.md` 决定打开、保存、冲突和关闭行为；`product/settings.md` 决定设置语义。参考图不能替代这些契约。
3. `visual/tokens.json` 决定精确颜色与初始逻辑参数，窗口用途和待补状态见窗口清单。尺寸目前是建议值，尚未经过原生量测。
4. 当前核心高保真图以 [design/manifest.json](../design/manifest.json) 中的 `baseline` 项为准；完整采用图组见 `boardAssetIds` 和[图板目录](../design/sources/ui-suite-v2.json)。同一图板只保存稳定文件名下的当前图片。
5. 架构已区分用户确认的实施方向与具体实现的准入条件。后续调整框架承载、渲染器、平台支持范围或资源契约时，更新现行架构与相关产品规格，不保留两套冲突规则；确认方向不等于实现或平台验收完成。

## 维护与检查

- 更新需求时同步调整验收映射；改变窗口或控件时同步更新视觉参数和材料索引。
- 文档链接应指向仓库内可恢复的文件，不依赖本次聊天的绝对路径。
- 持久设计材料与来源清单保留在仓库；具体运行截图、测量、测试输出进入本地或 CI 工件。
- 任务进展与单次验收结论不在本目录逐日追加。后续接入任务系统后，任务引用精确提交与证据，文档保留方法和现行契约。
