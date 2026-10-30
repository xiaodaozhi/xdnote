# 文档导航

本目录保存产品设计、视觉规范、架构决策依据与通用验收方法。当前文档是一份实现前基线，不是应用已完成的证明。

| 领域 | 文档 | 用途 |
| --- | --- | --- |
| 产品 | [需求](product/requirements.md) | 用户场景、需求来源、首版与后续范围 |
| 交互 | [交互规格](product/interaction-spec.md) | 打开路由、文档状态、窗口迁移、保存和关闭 |
| 控件 | [控件交互与焦点](product/control-interactions.md) | 点击反馈、Tab 外框、激活／取消后的清除与逻辑焦点 |
| 设置 | [设置规格](product/settings.md) | 主题与六种强调色、默认值、作用域和持久化 |
| 视觉 | [设计系统](visual/design-system.md) | 信息层级、跨界面公共组件一致性、缩放与材料权威 |
| 参数 | [tokens.json](visual/tokens.json) | 精确颜色、逻辑尺寸和字体参数 |
| 窗口 | [window-registry.json](visual/window-registry.json) | 窗口身份、建议尺寸、原稿与待补状态 |
| 工程 | [架构草案](engineering/architecture.md) | 分层候选、文档模型、保存和渲染边界 |
| 协作 | [产研与 GitHub 流程](engineering/workflow.md) | Issue、分支、PR、CI、验收及清理规则 |
| 参考 | [xdzip 实践](engineering/xdzip-practices.md) | 实际仓库依据及在 xdnote 中的采用方式 |
| 计划 | [开发路线](planning/roadmap.md) | 阶段顺序、产物和退出条件 |
| 验收 | [验收规格](qa/acceptance.md) | 需求映射、真实文件与平台验证方法 |
| 覆盖 | [界面清单](visual/screen-inventory.md) | 33 张图板、89 个页面与状态 |
| 素材 | [交付说明](visual/asset-handoff.md)、[素材几何](visual/asset-geometry.json) | 原图切片、SVG、透明 PNG 与使用约定 |
| 原稿 | [设计材料索引](../design/README.md)、[静态画廊](../design/index.html) | 当前图组、品牌候选、精确样张与提示词 |

## 文档权威

1. 用户已明确提出的需求优先，需求文档区分已确认方向、设计推导和待决策项。
2. `product/interaction-spec.md` 决定打开、保存、冲突和关闭行为；`product/settings.md` 决定设置语义。参考图不能替代这些契约。
3. `visual/tokens.json` 决定精确颜色与初始逻辑参数，窗口用途和待补状态见窗口清单。尺寸目前是建议值，尚未经过原生量测。
4. 当前核心高保真图以 [design/manifest.json](../design/manifest.json) 中的 `baseline` 项为准；完整采用图组见 `boardAssetIds` 和[图板目录](../design/sources/ui-suite-v2.json)。同一图板只保存稳定文件名下的当前图片。
5. 架构草案不是最终技术决策。选择框架、渲染器、平台支持范围后，更新草案与相关产品契约，不保留两套相互冲突的现行规则。

## 维护与检查

- 更新需求时同步调整验收映射；改变窗口或控件时同步更新视觉参数和材料索引。
- 文档链接应指向仓库内可恢复的文件，不依赖本次聊天的绝对路径。
- 持久设计材料与来源清单保留在仓库；具体运行截图、测量、测试输出进入本地或 CI 工件。
- 任务进展与单次验收结论不在本目录逐日追加。后续接入任务系统后，任务引用精确提交与证据，文档保留方法和现行契约。
