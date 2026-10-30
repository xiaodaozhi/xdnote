# 设计来源与导出

本轮界面图使用**内置 image_gen**生成与修订，没有使用 API/CLI 生成回退。当前目录保留实际提示词、生成时的参考记录、采用关系和导出源。

| 来源 | 用途 |
| --- | --- |
| [ui-suite-v2.json](ui-suite-v2.json) | 33 个图板、89 个状态、稳定路径和文本来源记录 |
| [v2-receipts](v2-receipts/01-workspace-light.json) | 逐图提示词、生成来源和摘要；旧版图片不保留 |
| [extensions](ui-suite-v2-extensions.json) | 菜单、文件路由、设置反馈、正文资源、导出过程和外部文件边界的新增设计输入 |
| [revisions](ui-suite-v2-revisions.json)、[refinements](ui-suite-v2-refinements.json) | 修订提示词与生成时的来源记录，不引用可用旧图 |
| [brand-mark.cjs](brand-mark.cjs)、[brand-placements.json](brand-placements.json) | 已确认的双色标记矢量源与图板局部替换区域 |
| [compare-titlebar-heights.cjs](compare-titlebar-heights.cjs) | 程序绘制 40／48 逻辑像素标题栏及双行文件信息的等比例对照图 |
| [standalone-chrome-edit.cjs](standalone-chrome-edit.cjs) | 独立窗口标题栏与工具栏局部修改记录；输入摘要用于防止对现有图板重复应用 |
| [align-standalone-titlebar.cjs](align-standalone-titlebar.cjs)、[compare-standalone-header.cjs](compare-standalone-header.cjs) | 参考工作区图板调整标题栏，按原图比例展示对照 |
| [ui-icon-paths.cjs](ui-icon-paths.cjs) | 本项目原创图标几何路径，不依赖图标字体 |
| [build-ui-assets.cjs](build-ui-assets.cjs) | SVG／透明 PNG、原图切片、图册、缩略图与静态画廊生成源 |
| [export-native-icons.py](export-native-icons.py) | 从已确认的应用图标 PNG 导出 ICO 与 ICNS |
| [finalize-ui-suite.cjs](finalize-ui-suite.cjs) | 将回执汇入采用目录、窗口／状态登记和材料身份 |
| [index-design-materials.py](index-design-materials.py) | 刷新材料清单和视觉参数的摘要，不生成归档包 |
| [check-ui-materials.cjs](check-ui-materials.cjs) | 检查本地链接、JSON、素材摘要、切片矩形、透明度与导出尺寸 |

完整窗口原稿仍是位图，不是可编辑 Figma 工程；独立控件与图标提供 SVG 源。来源回执中的 `prompt` 是实际生成文本；已移除的来源图仅记录生成时路径，不作为现存文件。图像重复生成不保证相同像素，因此以摘要识别当前图片。

## 重新导出

素材工具需要 Node.js 与 `sharp`；原生图标容器需要 Python 与 Pillow。它们仅用于设计文件处理，不是 xdnote 应用框架依赖。具备这些工具时，从仓库根目录运行：

```text
node design/sources/finalize-ui-suite.cjs
node design/sources/build-ui-assets.cjs
python design/sources/export-native-icons.py
python design/sources/index-design-materials.py
node design/sources/check-ui-materials.cjs
```

生成器读取[精确参数](../../docs/visual/tokens.json)和[素材几何](../../docs/visual/asset-geometry.json)。缩略图与图册使用本机系统字体，跨平台字形可能不同；不随素材复制字体文件。重新导出改变 PNG 字节时同步刷新摘要。

单独更新标题栏对照图时，运行 `node design/sources/compare-titlebar-heights.cjs`，再运行索引和检查命令。对照图的文字行高用于图示，实际平台效果仍待验证。

## 早期提示词

[imagegen-prompts.json](imagegen-prompts.json) 保留先前主工作区、独立文件和配色设置的提示词。早期青绿色稿不再保留图片；其完整请求来自对话，未补造不存在的原始提示词。

当前保留 33 张图板，均位于项目内，不依赖聊天缓存路径。同一图板只维护最新版本。生成图中的微小字形、配色近似和控件外观不覆盖产品行为及精确参数。
