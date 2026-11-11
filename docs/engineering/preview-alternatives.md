# 非 Chromium 预览方案评估

## 决策边界

产品不得使用 Chromium，包括 Qt WebEngine、CEF、Electron 或 Chromium 系统 WebView。Microsoft 明确说明 [WebView2 使用 Chromium 版 Edge](https://learn.microsoft.com/en-us/microsoft-edge/webview2/webview2-api-reference)，因此不能把它当作规避约束的轻量替代。产品已迁移到[原生阅读控件](native-preview-view.md)，并移除 WebEngine 的构建与运行依赖；本页保留选型依据，实际平台结果以 Issue／PR 为准。

保留 Qt 桌面、C++ 唯一原文／撤销、Scintilla 编辑器与 Rust pulldown-cmark 解析。不切换 Markdown 方言，不将预览文档变为第二份可编辑原文。首版需要基础语法、表格、任务列表、删除线、受控图片、正文选择／复制和跨文档标题定位，不要求网页兼容、JavaScript、任意 HTML 或所见即所得编辑。

## 候选比较

| 方案 | 可行性与复用 | 主要代价和限制 | 当前判断 |
| --- | --- | --- | --- |
| Qt `QTextBrowser`／`QTextDocument` 全量富文本 | 没有新增浏览器内核；Qt 已有文本选择、复制、基础富文本及辅助接口，可承接受控派生内容 | 全量布局随复杂长文增长；只支持 HTML／CSS 子集，任务框、代码复制、图片授权及部分标签需适配。标准接口存在不代表真实读屏已通过 | 适合作为功能基线或受限内容承载，不宜未经长文预算验证就作为最终方案 |
| Rust 全局块模型＋Qt 有界布局／绘制 | 不增加排版引擎；普通行内文本可用 `QTextLayout`，复杂块可用受限局部文档；只保留可见区及有上限的缓存 | 需实现块／行高映射、稳定滚动、跨块选区、可访问文本、表格及超大块内部布局；节数缓存不足以限制图片和单块内存 | **当前产品路线**；完整全局模型与原生控件已接入产品，受控分节试验仅作为早期依据 |
| litehtml 核心＋自有 Qt 容器 | 非 Chromium 的 C++ HTML／CSS 排版核心，较多现有 HTML／CSS 有机会复用，绘制可继续走 Qt | 需验证 Qt 文本塑形、选区／复制、资源回调、可访问树；完整 HTML 树也可能随长文增长。三平台、包体积和实际内存尚未实测 | 保留为第二候选；不能只凭上游“轻量”描述认定通过预算 |
| Qt Quick `Text`／`TextEdit`，或 `ListView` 分块委托 | 与 Qt Quick 承载一致；使用已有 Qt 文本能力；列表委托有机会限制可见项目 | 单个巨型 RichText 控件不等于虚拟化；富文本能力仍受 Qt 子集限制。分块后仍要处理跨委托选区、位置和辅助功能 | 是原生方案的界面承载变体，不是独立解决长文内存的内核选择；当前 QWidget 产品宜先用共享原生协调 |
| WebKit／平台系统 WebKit | 不依赖 Chromium，保留较完整 HTML 能力；WebKit 有 Windows、Apple 和 GTK／WPE 等端口 | 仍需浏览器内核、不同平台集成、资源隔离和发行维护。Windows 端口有独立工具链与依赖；未测得本项目内存和包体积 | 技术上可行，但本轮不优先于原生／轻量排版；不能假设它自然满足轻量目标 |

Qt 的 [HTML 子集](https://doc.qt.io/qt-6.8/richtext-html-subset.html)、[QTextLayout](https://doc.qt.io/qt-6.8/qtextlayout.html) 与 [Qt Quick Text](https://doc.qt.io/qt-6.8/qml-qtquick-text.html)界定现有能力。Qt Quick 文档还明确提示富文本可以加载图片，资源边界不能靠换控件自动获得。全量文档与有界缓存的取舍是架构推断，实际结论必须结合本项目输入和测量。

[litehtml 上游](https://github.com/litehtml/litehtml)说明，它解析并布局 HTML／CSS，实际字体、图像与绘制交给应用实现的容器回调；它不是完整浏览器替代。核心采用 [BSD-3-Clause 原文](https://github.com/litehtml/litehtml/blob/476f2eefc14e23e6b1d7e59de4bb7d8464072f04/LICENSE)，本轮审查的核心提交为 `476f2eefc14e23e6b1d7e59de4bb7d8464072f04`。未将其源码或二进制加入产品，Gumbo 等实际依赖须在引入具体版本时另外核验。

不能把 litehtml 核心的许可套用于 Qt 包装层。Qt 6.8.2 的 Qt Tools 对应 QLiteHtml 提交为 `2992a310640697325791a5494ca8f4d4552de368`；其[官方控件头文件](https://code.qt.io/cgit/playground/qlitehtml.git/plain/src/qlitehtmlwidget.h?id=2992a310640697325791a5494ca8f4d4552de368)标注 `LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0`，提供选择文本、链接、标题锚点及资源回调接口。若考虑直接使用，须单独核对包装层和 [Qt GPL 例外原文](https://raw.githubusercontent.com/qt/qtbase/v6.8.2/LICENSES/Qt-GPL-exception-1.0.txt)的分发条件；不能据此自行更改项目的 Apache-2.0 范围，也不笼统断言 GPLv3 与 Apache-2.0 不兼容。自有 Qt 适配层是另一条需计算实施成本的路径。

[WebKit Windows 端口文档](https://docs.webkit.org/Ports/WindowsPort.html)列出 cairo、libcurl 和额外构建工具。评估没有把 Windows 端口误写为不存在，也没有将其可构建等同于本项目最低系统、Qt 集成或内存通过。

## 本轮验证及其含义

[独立原生试验](../../experiments/native-preview-spike/README.md)只查找 Qt 6.8.2 Widgets，不构建或启动任何 Chromium 控制组。两种模式使用同一 Rust 库、全局解析结果及自有样本，递归核对 Qt DLL 闭包；测量包含派生模型和原生布局，不只统计某个渲染工作进程。

受控独立分节布局核对缓存上限、多个阅读位置、全局引用定义、源文摘要不变及资源回调拒绝。它只能证明受控样本下的成本方向，不能证明已完成通用 Markdown 分块。单个大段落、大代码块和大表格显式返回未支持，未按“测完即通过”处理；原生全量富文本的长文成本也保留。当前具体数值与证据见 [Issue #10](https://github.com/xiaodaozhi/xdnote/issues/10)，不将其写成产品内存保证。

当前截图还缺少彩色 emoji 字形、任务复选框、代码复制按钮及最终样式适配；窗口化视图未实现跨块选区与辅助技术。litehtml 和 WebKit 仅有一手资料与接口／许可审查，没有对应运行结果；候选排名包含明确的工程判断，不以文档审查冒充实测。

## 已选择路线与后续准入

用户已授权按工程判断推进，选择 Rust 全局语义模型＋Qt 原生按需布局主路线，保留 litehtml 核心＋自有适配为备选。[通用块与行内模型](native-preview-model.md)作为首个实施契约；具体布局控件仍需准入，不用当前按标题切节的试验直接替换产品。

1. 由一次全局解析提供段落、标题、列表、引用、代码和表格的块结构、行内语义、引用定义及 UTF-8 范围；带上文档、文本和路径代次。不能独立解析源文切片来模拟分块。
2. 同时约束模型字节、布局缓存字节／条目、图片解码和超大块。覆盖无标题长文、超长段落／代码、宽表与大表、嵌套列表及跨块引用；不靠新增未确认文件限制掩盖缺口。
3. 以当前设计验证主题、字体和 emoji 回退、静态任务框、代码与表格横向滚动、复制、全局选择、链接与标题位置。接入已有受控资源、唯一会话与版本核验，不另建读写或授权通路。
4. 在固定身份和环境下补充空闲、长文、持续更新、图片、跨窗口及关闭回收的整进程树证据，再提出产品预算。Windows／macOS／Linux、真实读屏和桌面交互仍各自记录，不能由 Windows 离屏结果替代。
5. 承载准入后单独实施产品迁移，移除 WebEngine 链接、SDK 模块、子进程／资源设置与过时部署路径，重新验证实际文件清单。旧 WebEngine 部署草稿不直接合并。
