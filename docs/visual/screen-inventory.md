# 界面设计覆盖清单

本轮以 **33 张图板、89 个页面与状态**覆盖 xdnote 当前产品范围。所有图均为静态设计；组件状态、六色和明暗变体进一步覆盖共享控件，未开始应用实现。

[完整画廊](../../design/index.html) · [总览图](../../design/hifi/v2/overview.png) · [切图与素材](../../design/assets/README.md)

| 编号 | 图板 | 页面／状态标识 | 设计范围 |
| --- | --- | --- | --- |
| 01 | [主工作区，浅色双栏](../../design/hifi/v2/boards/01-workspace-light.png) | `workspace-split-light` | P0 |
| 02 | [主工作区，深色双栏](../../design/hifi/v2/boards/02-workspace-dark.png) | `workspace-split-dark` | P0 |
| 03 | [首次打开与空白文档](../../design/hifi/v2/boards/03-welcome-blank.png) | `welcome`、`untitled-document` | P0 |
| 04 | [阅读模式与专注写作](../../design/hifi/v2/boards/04-reading-focus.png) | `workspace-reading`、`focus-source` | P0 |
| 05 | [独立文件，浅色](../../design/hifi/v2/boards/05-standalone-light.png) | `standalone-reading-light`、`standalone-editing-light` | P0 |
| 06 | [独立文件，深色](../../design/hifi/v2/boards/06-standalone-dark.png) | `standalone-reading-dark`、`standalone-editing-dark` | P0 |
| 07 | [快速打开与查找替换](../../design/hifi/v2/boards/07-search-find.png) | `quick-open`、`find-replace` | P1 |
| 08 | [最近、收藏与检索状态](../../design/hifi/v2/boards/08-navigation-states.png) | `recent-files`、`favorite-files`、`search-empty`、`workspace-empty` | P1 |
| 09 | [文件与标签菜单](../../design/hifi/v2/boards/09-file-tab-menus.png) | `file-context-menu`、`tab-context-menu`、`tab-overflow`、`document-info` | P1 |
| 10 | [编辑工具与插入面板](../../design/hifi/v2/boards/10-insert-popovers.png) | `heading-format`、`insert-link`、`insert-image`、`insert-table` | P0 |
| 11 | [通用与编辑器设置](../../design/hifi/v2/boards/11-settings-general-editor.png) | `settings-general`、`settings-editor` | P0 |
| 12 | [外观与阅读设置](../../design/hifi/v2/boards/12-settings-appearance-reader.png) | `settings-appearance-light`、`settings-reader` | P0 |
| 13 | [快捷键与关于](../../design/hifi/v2/boards/13-settings-shortcuts-about.png) | `settings-shortcuts`、`settings-about` | P0 |
| 14 | [外观设置，深色](../../design/hifi/v2/boards/14-settings-dark.png) | `settings-appearance-dark` | P0 |
| 15 | [关闭与未保存保护](../../design/hifi/v2/boards/15-unsaved-close.png) | `unsaved-single`、`unsaved-multiple`、`close-while-saving`、`discard-reload-confirm` | P0 |
| 16 | [外部修改与内容比较](../../design/hifi/v2/boards/16-external-conflict.png) | `compare-external`、`external-dirty-dialog`、`external-clean-banner` | P0 / P1 |
| 17 | [保存失败与只读状态](../../design/hifi/v2/boards/17-save-readonly-errors.png) | `save-permission`、`save-disk-full`、`readonly-document`、`save-outcome-unknown` | P0 |
| 18 | [文件管理与失效入口](../../design/hifi/v2/boards/18-file-lifecycle.png) | `rename-inline`、`duplicate-name`、`trash-confirm`、`missing-file` | P0 / P1 |
| 19 | [编码、大文件与加载](../../design/hifi/v2/boards/19-encoding-large-loading.png) | `encoding-unsupported`、`large-file-choice`、`loading-document`、`preview-failed` | P0 / P1 |
| 20 | [PDF 与 HTML 导出](../../design/hifi/v2/boards/20-export.png) | `export-pdf`、`export-html` | P1 |
| 21 | [复杂 Markdown 阅读版式](../../design/hifi/v2/boards/21-rich-markdown.png) | `rich-markdown-reading` | P0 |
| 22 | [窄窗口与大字号](../../design/hifi/v2/boards/22-compact-large-text.png) | `compact-workspace`、`settings-large-font` | P0 |
| 23 | [浅色共享组件状态](../../design/hifi/v2/boards/23-components-light.png) | `components-light` | design-system |
| 24 | [深色共享组件状态](../../design/hifi/v2/boards/24-components-dark.png) | `components-dark` | design-system |
| 25 | [六色与明暗组合](../../design/hifi/v2/boards/25-accent-matrix.png) | `accent-theme-matrix` | design-system |
| 26 | [非阻塞反馈与边界状态](../../design/hifi/v2/boards/26-feedback-states.png) | `save-status`、`notifications`、`folder-unavailable`、`new-empty-source`、`disabled-tooltip`、`external-link` | P0 |
| 27 | [跨平台外壳与系统交接](../../design/hifi/v2/boards/27-platform-chrome.png) | `platform-chrome` | platform-reference |
| 28 | [应用菜单](../../design/hifi/v2/boards/28-application-menus.png) | `menu-file`、`menu-edit`、`menu-view`、`menu-window-help` | P0 |
| 29 | [打开与窗口路由](../../design/hifi/v2/boards/29-open-route-feedback.png) | `drag-file-target`、`batch-open-partial`、`same-file-existing`、`move-session-failed` | P0 |
| 30 | [设置检索与保存反馈](../../design/hifi/v2/boards/30-settings-feedback.png) | `settings-search-results`、`settings-search-empty`、`settings-write-error`、`settings-protected-close` | P0 / P1 |
| 31 | [正文交互与资源边界](../../design/hifi/v2/boards/31-content-interactions.png) | `source-ime-selection`、`image-resource-states`、`wide-code-table`、`outline-no-headings` | P0 |
| 32 | [导出过程与结果](../../design/hifi/v2/boards/32-export-feedback.png) | `export-in-progress`、`export-complete`、`export-failed`、`export-cancelled` | P1 |
| 33 | [外部文件变化边界](../../design/hifi/v2/boards/33-external-file-edges.png) | `external-deleted-dirty`、`external-renamed-clean`、`file-busy-save`、`reopen-encoding-dirty` | P0 / P1 |

## 覆盖与优先级

图板覆盖工作区、独立阅读与编辑、六类设置、菜单、导航、查找、插入、文件打开与保存、外部变化、编码与大文件、复杂 Markdown、资源加载、导出和系统界面交接。浅深色布局共用参数，六种强调色分别选择。

`P0` 表示基础闭环的设计，`P1` 表示提前绘制的后续方案；出现图板不自动提升开发优先级。内容比较和未知编码等兼容设计仍需技术决策。产品行为以[需求](../product/requirements.md)、[交互](../product/interaction-spec.md)和[设置](../product/settings.md)为准。

系统文件选择器、默认应用和平台窗口装饰归操作系统管理；图板 27 是交接示意，不是三平台原生截图。账户、云同步、AI、插件市场、所见即所得和崩溃恢复不进入本轮既定范围。

## 切图交付

每个页面／状态均有原图矩形切片，记录源图摘要和像素坐标；这些切片用于设计对照。可复用素材另提供原始 SVG、透明 PNG 1×／2×／3×、文字内容区和可拉伸边距，精确色值读取[tokens.json](tokens.json)。按钮文字、正文和系统标题栏继续作为文本或平台界面，不烘焙成通用控件皮肤。

当前图板与生成提示词见[来源目录](../../design/sources/README.md)；旧版图片不再保存。原稿中的少量字形与颜色近似不能覆盖精确参数；素材索引将界面参考切片和矢量控件明确标注用途。
