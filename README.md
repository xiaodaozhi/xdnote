# xdnote

一款面向 Windows、macOS 和 Linux 的 Markdown 阅读与编辑桌面应用，让打开、阅读和修改本地文档保持简单。

**当前阶段：产品设计与技术验证。** 仓库已整理需求、交互、视觉规范和高保真参考图，并保留独立的技术试验；尚无正式产品源码或安装包。下文描述目标能力，不代表已经实现。

![xdnote 主要界面设计参考](design/hifi/v2/featured.png)

本轮已制作 33 张静态图板，覆盖 89 个页面与状态，并输出对应切图、SVG 与透明 PNG 素材。设计图使用石墨灰展示，保留六种可选强调色；折光品牌与应用图标方向已获用户确认，原生平台显示尚待验证。

[浏览完整设计图](design/index.html) · [查看界面覆盖清单](docs/visual/screen-inventory.md) · [查看设计素材](design/assets/README.md)

## 产品方向

- **随手打开一个文件**：通过系统双击打开 `.md`、`.markdown`，直接进入独立阅读窗口，无需先创建或导入工作区。
- **按需开始编辑**：独立窗口在阅读与编辑间切换，主工作区提供编辑、双栏和阅读视图。
- **管理一组文档**：工作区承载文件导航和多标签，独立文件可移入工作区继续处理，同一文件的未保存内容保持连续。
- **可靠地保存**：区分未保存、保存中、保存失败与外部修改冲突；关闭和退出保护未保存内容。
- **选择自己的外观**：浅色、深色、跟随系统与六种强调色分别选择，配色在主界面、独立窗口和设置窗口中保持一致。

需求来源、首版边界和后续能力见[产品需求](docs/product/requirements.md)。

## 从哪里开始

| 我想了解 | 入口 |
| --- | --- |
| 产品定位、首版范围 | [产品需求](docs/product/requirements.md) |
| 双击打开、多窗口、保存与关闭行为 | [交互规格](docs/product/interaction-spec.md) |
| 配色、主题和其他偏好 | [设置规格](docs/product/settings.md) |
| 颜色、密度、组件和窗口约定 | [视觉规范](docs/visual/design-system.md)、[视觉参数](docs/visual/tokens.json)、[窗口清单](docs/visual/window-registry.json) |
| 技术方案与需要先验证的问题 | [架构草案](docs/engineering/architecture.md) |
| 从 xdzip 借鉴的实际做法 | [xdzip 实践参考](docs/engineering/xdzip-practices.md) |
| 阶段顺序与验收要求 | [开发路线](docs/planning/roadmap.md)、[验收规格](docs/qa/acceptance.md) |
| Issue、分支、PR 与 CI | [产研流程](docs/engineering/workflow.md)、[贡献指南](CONTRIBUTING.md) |
| 查看设计原稿和素材 | [设计材料](design/README.md)、[高保真图](design/hifi/README.md)、[素材交付](docs/visual/asset-handoff.md) |

完整索引见 [docs/README.md](docs/README.md)。

## 仓库结构

```text
xdnote/
├── README.md
├── AGENTS.md                  # 协作与材料维护约定
├── docs/
│   ├── product/               # 需求、交互、设置
│   ├── visual/                # 视觉规则、参数、窗口登记
│   ├── engineering/           # 架构草案与可复用工程实践
│   ├── planning/              # 阶段与退出条件
│   └── qa/                    # 可复用验收规格
├── design/
│   ├── hifi/v2/              # 当前完整图组、总览和缩略图
│   ├── assets/ui-v2/         # 矢量、透明 PNG 与原图参考切片
│   ├── assets/palettes/      # 前期配色候选样张
│   ├── brand-proposals/      # 已选品牌及其他候选方向
│   ├── sources/              # 图像提示词、回执与素材导出源
│   ├── index.html            # 静态材料画廊
│   └── manifest.json          # 材料身份、尺寸、摘要与用途
└── experiments/              # 与产品实现明确区分的技术验证源码
```

应用目录随技术验证结果建立，不预建没有实现的模块。当前架构草案把 xdzip 的 Qt Quick、薄 C++ 与 Rust 分层作为候选方向，尚未锁定框架、渲染器和版本。

技术验证入口：[Qt 文档与窗口试验](experiments/qt-document-spike/README.md) · [Scintilla 文本缓冲区试验](experiments/scintilla-buffer-spike/README.md)。两者均不是产品实现。

## 维护方式

产品行为以 `docs/product` 为准，精确视觉值以 `docs/visual` 为准，位图用于布局和风格参考。设计图中尚未定义的导出、收藏等入口不自动进入首版承诺。

设计原稿、素材和提示词是持久输入，保留在 `design`；不在源码目录保存设计导出 ZIP。构建、运行截图、测量与检查报告进入忽略的 `build/`、`target/` 或 `dist/`。任务进度使用 GitHub Issues，文档维护可复用规则，不累积执行流水账。

平台最低版本、架构矩阵、签名与发行策略均需在对应阶段确定，详见[开发路线](docs/planning/roadmap.md)。

## 许可范围

本仓库的自有软件代码（包括工具脚本和技术试验代码）采用 [Apache-2.0](LICENSE)。文档、品牌标识、图标、设计图及其他设计素材的许可另行决定；素材即使嵌入代码，其设计内容也不因此获得代码许可。第三方内容按其自身条款处理。详情与待决事项见[许可范围与依赖](docs/engineering/licensing.md)。
