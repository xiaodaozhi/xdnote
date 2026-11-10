# 原生只读预览与 WebEngine 成本对照

这是独立候选试验，不替换产品预览，也不决定最终承载。编辑原文与撤销仍由 Scintilla 会话负责；这里的 `QTextBrowser`／`QTextDocument` 只消费 Rust 生成的只读 HTML，不能成为原文权威。

两个可执行文件使用同一份生成 Markdown、同一个已构建 Rust 静态库和同一份解析结果。原生候选只链接 Qt Widgets；控制组另链接 Qt WebEngine Widgets。脚本只读 PE 导入闭包，拒绝原生目标意外依赖 WebEngine。依赖 DLL 大小仅统计 Qt 导入闭包，不含插件、资源、运行库或完整产品功能，因此不能称为最终安装体积。

## 可复现入口与证据

当前采样器仅实现 Windows，固定使用本项目 Qt 6.8.2、默认 VS2022 Build Tools，以及 `build/cycle-10-background/` 中通过身份核对的 Rust 静态库。系统工具和本项目工具的归属仍由主入口检查；缺少前置构建时拒绝运行，不跨项目回退。先用[后台验证入口](../../docs/qa/background-validation.md)构建该目录，再首次初始化试验输出目录：

```text
python scripts/cleanup_cycles.py --init cycle-10-preview-comparison
python experiments/native-preview-spike/compare.py
```

同一试验可以重新运行第二行，使用稳定文件名替换日志、截图和 `comparison.json`。构建和进程均使用临时环境，Qt 平台固定 offscreen，不操作真实桌面或用户文档。

每组使用 10 KiB、1 MiB、2 MiB 的自有密集语法样本，核对两个候选的源文摘要相同、中文／emoji 字符与转义 HTML 文本保留。截图在加载后额外等待 200 毫秒并检查存在非空绘制；时间字段包含这段等待，是单次候选观察，不是冷启动或稳定分布。WebEngine 读取自生成临时 HTML，避免 `setHtml` 的 data URL 大小限制与渲染成本混淆。

采样器每约 50 毫秒枚举本次诊断进程及其后代，只读取其内存计数，不记录其他进程的名称、命令行或正文。报告分别保留进程树峰值私有提交量和工作集合计；私有提交可能包含已换出的分配，工作集合计可能重复计算共享页，两者都不能不加说明地称为实际物理内存。峰值统计包含解析及布局，子进程也纳入；单次采样仍可能遗漏极短尖峰。超时仅结束自有诊断进程树。

## 能力与准入限制

[QTextBrowser](https://doc.qt.io/qt-6.8/qtextbrowser.html) 支持只读富文本与链接；其 [HTML／CSS 子集](https://doc.qt.io/qt-6.8/richtext-html-subset.html)不是浏览器排版实现。试验禁用链接自动打开，Rust 输出中的文档 URL 已惰性化；这不代表实际产品资源授权、标题定位、复制交互或读屏接口已接入。

基础富文本能展示中文、标题、列表、表格和代码文本，但不能直接复用浏览器 CSS、任务复选框、代码复制按钮或现有 Web 交互。原生全量富文本也须完成长文内存与布局准入，不因去掉 Chromium 就声称低内存。若继续原生路线，应单独验证按块布局、可见区域渲染、受控图片、横向溢出、选区与复制、标题定位和当前设计的样式映射，再决定是否进入产品。

具体运行数值只进入忽略的 `build/` 证据及 Issue／PR，不在本页累积机器结果。三平台、真实桌面、辅助技术、完整产品内存与最终发布依赖仍未由本试验覆盖。
