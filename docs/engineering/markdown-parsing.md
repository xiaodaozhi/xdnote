# Rust 原生 Markdown 解析方案

## 结论与适用范围

Rust 原生解析适合当前 xdnote，并已接入产品：继续使用固定的 `pulldown-cmark`，由自有适配生成只读语义模型，Qt 负责排版、绘制与交互。当前没有足够依据更换解析器或从头自研 CommonMark 内核。本结论不扩大文件容量、Markdown 方言或平台验收范围；替换候选仅作资料比较，没有本项目的同口径性能实测。

解析器识别标题、段落、列表、代码、链接等语义；字体、间距、表格布局、滚动、选择和图片显示由原生阅读控件处理。解析器可以输出 HTML，但这不要求使用浏览器；产品读取的是二进制语义模型。改变解析器不会自动改善阅读样式。

## 现有实现与集成可行性

- [Rust crate](../../crates/markdown/Cargo.toml) 固定 `pulldown-cmark = 0.13.6`，通过 Cargo 锁文件和既有许可清单管理依赖。方言是 CommonMark 加表格、任务列表、删除线；脚注、数学、MDX、Wiki 链接等不因库支持便自动启用，也不称为完整 GitHub 渲染兼容。
- [语义模型构建器](../../crates/markdown/src/native_model.rs) 对完整 UTF-8 快照进行全局解析，从事件及源码范围建立前序扁平树。跨块引用由同一模型解析实例处理，不按可见片段独立解析；标题 ID 与独立标题分析共用生成规则。产品预览已通过[共享分析流水线](preview-pipeline.md)在同一次全局事件遍历中生成模型与精确引用信息；局部目标拼写仍重新准入，独立扫描入口继续保留。
- [C ABI](../../apps/desktop/src/MarkdownBridge.h) 返回带状态和长度的 Rust 缓冲区；Qt 接收端核对编码、树和 UTF-8 边界，共享持有只读结果，最后由 Rust 的释放入口回收。C++ 不直接解释 Rust 枚举或依赖 Rust ABI，也不释放 Rust 分配。错误和可捕获的 panic 转换为失败结果；这不等于任意分配失败或进程故障都可恢复。
- [预览协调器](../../apps/desktop/src/MarkdownPreview.cpp) 在 Qt Concurrent 工作线程处理不可变快照，UI 线程发布当前结果；文档身份、修订、请求、路径上下文和资源代次参与门控。Rust 不持有第二份可写正文或撤销，解析结果不能自行读盘或执行链接。
- [原生模型](native-preview-model.md)、[阅读控件](native-preview-view.md)和[段落滚动映射](scroll-synchronization.md)已在同一产品路径使用源码范围。现有 Windows、Linux 和 macOS 构建及定向证据说明这条集成路线可实施，不代表最低系统、完整发行或真实辅助技术均已验收。

原始 HTML 作为字面内容展示。图片、文件链接和外部链接继续经过各自资源与导航契约；解析成功和 Rust 的内存安全不能代替目标授权。

## 候选比较

以下能力依据上游项目和公共 API；“成本”与“判断”是结合本项目契约的工程推断，并非候选实测排名。上游移动文档不作为升级依据；实际引入时须固定具体版本、特性及传递依赖。

| 方案 | 输出与来源位置 | 增量／取消边界 | 与当前产品的适配及判断 |
| --- | --- | --- | --- |
| [pulldown-cmark](https://github.com/pulldown-cmark/pulldown-cmark) | Rust 事件迭代器，[`into_offset_iter`](https://docs.rs/pulldown-cmark/latest/pulldown_cmark/struct.Parser.html#method.into_offset_iter) 返回事件及源文范围；可按需生成自有模型 | 当前使用的 API 从完整字符串构造解析器；事件迭代不是旧树复用。公共接口没有本项目可直接使用的硬取消保证 | 已适配方言、UTF-8 范围、引用、标题及 FFI；继续作为主方案。迭代接口也不表示解析器内部不保留结构或没有峰值分配 |
| [Comrak](https://comrak.ee/) | Rust CommonMark／GFM 解析器，[`parse_document`](https://docs.rs/comrak/latest/comrak/fn.parse_document.html) 返回 arena AST；[`Sourcepos`](https://docs.rs/comrak/latest/comrak/nodes/struct.Sourcepos.html) 提供首尾行列 | 本次核对的公共入口接收完整正文，没有据此证明编辑增量复用或可中断解析 | 适合更丰富的 AST 操作与扩展；替换需重新适配节点、行列到字节范围、标题与引用行为。AST 和 arena 的实际内存须测量，不能直接断言更慢或更大；保留为语义需求变化时的候选 |
| [markdown-rs](https://github.com/wooorm/markdown-rs)（crate `markdown`） | Rust 状态机，可生成带位置的 mdast，支持 CommonMark／GFM 及 MDX、数学、frontmatter 等扩展 | 公共 [`to_mdast`](https://docs.rs/markdown/latest/markdown/fn.to_mdast.html) 接收全文；有位置不等于提供编辑增量协议 | 适合复杂 AST 处理和相关生态互通；当前无 MDX 需求，新增适配、输出树与依赖的收益尚未成立，不优先替换 |
| [tree-sitter Markdown](https://github.com/tree-sitter-grammars/tree-sitter-markdown/blob/split_parser/README.md) | 块与行内两套语法树，Rust 包装使用 Tree-sitter 运行时与生成的 C 语法，不是纯 Rust Markdown 内核 | [旧树编辑与重解析](https://tree-sitter.github.io/tree-sitter/using-parsers/3-advanced-parsing.html)支持增量复用；仍需维护两层树和输入版本 | 上游明确以语法高亮为主要目标，并指出不宜用于正确性重要的场景。不作为预览、引用维护或保存改写的语义权威；如以后研究高亮，应单独验证，避免引入两套互相矛盾的方言 |

Comrak 的来源列默认按 UTF-8 字节计数，也可配置字符计数，见 [`LineColumn`](https://docs.rs/comrak/latest/comrak/nodes/struct.LineColumn.html)。无论哪种表示，都必须按同一快照转换，不能直接当成 Qt UTF-16 位置。pulldown 的源文范围也不意味着实体解码、转义、代码空白和显示字符逐字对应；双栏当前承诺段落语义映射，不将它扩大为逐字命中保证。

许可方面，现用 pulldown-cmark 为 MIT，已记录在[第三方声明](../../THIRD_PARTY_NOTICES.md)；候选上游分别声明 Comrak 为 [BSD-2-Clause](https://github.com/kivikakk/comrak/blob/main/COPYING)、markdown-rs 为 [MIT](https://github.com/wooorm/markdown-rs/blob/main/license)、tree-sitter Markdown 语法为 [MIT](https://github.com/tree-sitter-grammars/tree-sitter-markdown/blob/split_parser/LICENSE)。这里只比较直接项目声明，不代表候选完整依赖闭包或实际交付许可已经核验；未引入的候选不写入产品第三方清单。

## 全文解析、取消与资源边界

当前源文上限是 2 MiB，语义编码结果上限是 64 MiB。后者仅约束派生输出，不是解析器峰值、进程总内存或应用预算。密集语法、嵌套、引用展开、字符串池、Qt 投影和布局都会产生额外成本；不能只凭 Rust、事件流或文件字节数宣布轻量目标达成。

现有协调器合并待处理快照，并拒绝发布过期结果；[流水线](preview-pipeline.md)已加入解析事件和阶段边界的协作取消。关闭窗口后不发布结果，旧任务在下一个检查点退出；这不能同步中断解析器构造、单次迭代、读盘、解码或投影内部的昂贵调用。当前没有必要仅为资料研究新增工作进程；如果实际测量暴露无法接受的阻塞，应分别评估协作取消、解析内核调整和可终止工作进程，说明开销及回收证据。

真正的增量解析需要处理全局影响：末尾引用定义可能改变前部链接，围栏或列表结构修改可能影响后续块，重复标题会改变锚点序号。不得用按行、固定长度或可见区域切片后独立解析来冒充正确增量。继续测量共享全局分析、资源准备、投影和布局成本；没有瓶颈证据前不引入第二套解析树。

从头自研也能实现，但需承担 CommonMark 兼容、扩展组合、畸形输入、复杂度防护、源码映射、模糊测试及长期维护。现有成熟内核与自有适配能满足当前需求，尚无值得承担这些成本的已证实缺口。

## 复核方法与后续触发条件

继续使用现有 Rust 回归覆盖中文／emoji、实体、CRLF、嵌套、跨块引用、代码中的伪链接、标题冲突、最大单块、输出配额、无效 UTF-8 与 FFI 释放；按[后台验证口径](../qa/background-validation.md)测量自生成的 10 KiB、1 MiB、2 MiB 输入及超限／密集引用拒绝。记录源码与样本摘要、预热、逐次耗时和分位值，不把不同机器或不同语义的上游跑分直接排名。

现有 `model` 探针计时包含 Rust 模型解析、Qt 接收和原生内容投影，不包含实际排版／绘制；工作进程峰值工作集也不是纯解析器或完整应用内存。候选若进入替换准入，必须使用相同语料、扩展开关、输出职责、构建优化和内存口径，另外验证段落映射、引用改写、标题导航及旧结果门控；不能仅比较 HTML 转换时间。

只有出现可复现的语法错误、无法满足的来源位置需求、已测得的更新延迟／资源瓶颈，或已确认的新扩展需求时，再开启替换或增量专题。先定位解析、资源准备、投影还是排版；已接入同版本分析与受控派生缓存，后续仍按分阶段数据评估收益，不预先保证优化幅度。单次验证数字与未执行项保存在对应 Issue／PR 和构建证据中，本页保留选择依据与复核方法。
