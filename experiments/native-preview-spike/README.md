# 非 Chromium 原生预览可行性试验

本试验仅链接 Qt 6.8.2 Core／Gui／Widgets，比较全量 `QTextBrowser` 与有界分节布局。没有 WebEngine 控制组，不启动 Chromium 或产品窗口。编辑原文与撤销仍由现有 Scintilla 会话负责，试验的 `QTextDocument` 只持有只读派生内容。

## 输入与范围

使用同一个已核对身份的本项目 Rust 静态库，全局解析同一份 10 KiB、1 MiB、2 MiB 自有密集语法样本。样本的引用定义在文末，前面的分节引用同一个定义；分节发生在全局解析之后，合并分节 HTML 必须与整体结果逐字符一致，不独立重解析 Markdown 切片。

当前分节器**仅适用于已知独立 H1 分节的受控样本**：它依据同一 Rust 标题映射查找生成 HTML 的边界，不是任意 Markdown 的 AST 分块实现。嵌套结构、跨块选区、可访问文本、精确像素滚动及产品资源／链接接口仍未实现。分节视图使用节序号滚动，不能冒充最终连续滚动体验。

布局缓存最多八份文档；单节派生 HTML 超过 65,536 个字符时，试验返回 `supported=false`，不布局或截断为通过。另用单个大段落、大代码块、大表格分别暴露这个缺口。该试验额度不是批准给产品的新限制，正式方案必须处理块内虚拟化或提出明确的准入调整。

两种模式共用项目系统字体定义，验证中文／emoji 字符串保真、跨节引用解析、源文摘要不变，以及本地／远程图片资源回调被拒绝。截图只证明出现了绘制，不能替代字体／彩色 emoji 字形、任务框、复制按钮、主题或完整视觉验收。现有 HTML 在 Qt 富文本子集下仍需样式与语义适配。

## 执行与证据

当前采样器仅支持 Windows／VS2022，Qt SDK、虚拟环境及 Rust 产物均来自本项目。`--source-build` 指向已经成功构建且身份一致的本项目目录，只复用其 Rust 静态库，不启动其应用或依赖旧预览。以下默认使用主线构建 `build/cycle-72-completion`；可显式提供另一个匹配目录。

```text
python scripts/cleanup_cycles.py --init cycle-10-native-evaluation
python experiments/native-preview-spike/evaluate.py --source-build build/cycle-72-completion
```

已有输出目录时只运行第二行。`--qt-prefix`、`--build-dir` 和 `--vcvars` 可显式指定准备好的项目 SDK、输出及系统编译器。配置只查找 Qt Widgets；启动临时设置 DLL／插件／系统字体和 offscreen 平台，不调用产品运行入口，不修改系统 PATH。工具递归核对实际 Qt DLL 导入，发现 WebEngine、WebView、Chromium 或 CEF 依赖即拒绝继续。

`build/cycle-10-native-evaluation/evaluation.json` 保存源码摘要、Rust 库身份、构建依据、Qt DLL 闭包、各次结果、布局缓存与覆盖缺口；截图和原始日志使用稳定文件名。单次测量包含全局解析、派生模型和布局，每约 50 毫秒统计整个自有进程树。峰值私有提交可能包含换出分配，工作集合计可能重复共享页；它们不是整应用常驻物理内存。首帧字段包含强制抓图／编码成本，不能直接称为交互延迟。

离屏六组正常样本与三组未支持大单块均需得到与预期相符的结果。`complete=true` 表示试验流程完成，**不表示三个缺口已经解决**。具体数值与当前提交关联在 Issue／PR，说明文档不累积机器运行记录。
