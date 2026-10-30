# 切图与可复用素材

本轮素材位于 [ui-v2](ui-v2/manifest.json)，共 **86 个图标、310 个控件变体、141 张参考切片**，另有语义反馈、空态插画、已确认的品牌方向与预览图册。

[完整画廊](../index.html) · [素材清单](ui-v2/manifest.json)

## 目录

| 目录 | 内容与格式 |
| --- | --- |
| `ui-v2/icons/svg` | 86 个原创线性图标，`currentColor`，16 逻辑像素，24 坐标系 |
| `ui-v2/icons/png/light`、`dark` | 相同图标浅深色 PNG，1×／2×／3× |
| `ui-v2/components/{theme}/{accent}` | 主要按钮、复选框、单选、开关、分段、选中行、标签、色块与焦点 |
| `ui-v2/components/{theme}/neutral` | 次要／无底／危险按钮、输入框、未选与禁用控件 |
| `ui-v2/feedback`、`surfaces` | 保存／警告／错误、未保存点、提示底形、弹出面、滚动条与分隔线 |
| `ui-v2/empty-states` | 8 种无文字空态，每种提供浅深色 |
| `ui-v2/brand` | 折光标记、字标与应用图标，SVG、PNG、ICO、ICNS；原生平台效果待验收 |
| `ui-v2/reference-slices` | 89 个页面／状态的原图矩形切片 |
| `ui-v2/reference-components` | 浅深色各 26 个组件局部的原样裁切 |
| `ui-v2/previews` | 图标、控件和精确六色图册 |

品牌素材入口：[浅色标记](ui-v2/brand/mark-light.svg)、[深色标记](ui-v2/brand/mark-dark.svg)、[浅色字标](ui-v2/brand/logo-light.svg)、[深色字标](ui-v2/brand/logo-dark.svg)、[应用图标 SVG](ui-v2/brand/app-icon.svg)、[ICO](ui-v2/brand/app-icon.ico)、[ICNS](ui-v2/brand/app-icon.icns)。

## 使用规则

可复用图形具有透明背景。`@1x`、`@2x`、`@3x` 对应相同逻辑尺寸的不同采样。SVG 源可直接调整颜色；PNG 已按主题／配色导出。文字、光标、选区和平台窗口装饰仍由后续应用绘制，不使用截图文本替代。

控件源有透明焦点留白。请读取清单的 `logicalCanvas`、`coreRect`、`contentRect` 和 `nineSlice`／`stretch`，只拉伸平直中心区域；图标与选择标记不拉伸。共享参数来自 [tokens.json](../../docs/visual/tokens.json) 和 [asset-geometry.json](../../docs/visual/asset-geometry.json)。

中性输入框和次要按钮可叠加对应强调色的通用焦点层；六色不复制六套布局。保存、警告和错误始终使用独立语义色。图标至少提供 32 逻辑像素的命中区域。

141 张参考切片按 `sourcePath`、`sourceSha256`、`pixelRect` 追溯，未经缩放，保留原图中的背景和文字，只用于视觉对照。它们与透明的可复用控件是两类文件。

## 预览

[精确六色与明暗](ui-v2/previews/accent-matrix.png) · [浅色控件](ui-v2/previews/components-light.png) · [深色控件](ui-v2/previews/components-dark.png) · [浅色图标](ui-v2/previews/icons-light.png) · [深色图标](ui-v2/previews/icons-dark.png)

原稿由内置 image_gen 生成；可复用图形由本项目原创几何路径绘制，没有从第三方库或 xdzip 搬入图标、品牌或字体。未分发字体。品牌方向已确认，仓库整体许可尚待项目决策。来源、导出方式和限制见[来源说明](../sources/README.md)及[交付规格](../../docs/visual/asset-handoff.md)。

早期[六色候选样张](palettes/accent-color-candidates.png)保留为设计过程材料，精确值仍以共享参数为准。
