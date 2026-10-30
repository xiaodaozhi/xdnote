# D1：Qt 文档与窗口技术试验

本目录是可复现的技术验证代码，不是 xdnote 产品或可交互设计原型。它验证 Qt 候选方案的部分关键行为，不预先决定最终界面框架、解析器、文件模型或发行工具链。

## 试验覆盖

| 验证点 | 方法和边界 |
| --- | --- |
| 同一文档跨窗口 | 两个 Qt Quick `TextEdit` 接入同一 `QTextDocument`，在隐藏的独立窗口与工作区窗口间接管，核对正向／反向选区、文本及撤销重做 |
| 输入法组合 | 发送合成的 `QInputMethodEvent`，组合期间拒绝接管，提交后继续；不代表真实中文输入法已验收 |
| Markdown | 对标题、任务列表、表格、围栏代码、原始 HTML 和图片请求做有限样本检查，预览阶段不复用源文的撤销栈 |
| 资源访问 | 预览文档覆盖资源加载器，测试中一律返回占位图，不访问本地或网络资源；真实图片服务尚未实现 |
| 文件保存 | 使用自建临时文件检查 UTF-8 BOM、CRLF、无断行空格、外部修改、目标删除与保存期间继续编辑 |
| 基准 | 自生成 10 KiB、1 MiB、10 MiB Markdown，单次测量隐藏窗口、读取、文本插入、解析、排版和进程内存 |

Qt Quick 允许调用方将 `QTextDocument` 绑定到 `TextEdit`，并由调用方持有文档；详见 [Qt 6.8 QQuickTextDocument](https://doc.qt.io/qt-6.8/qquicktextdocument.html#setTextDocument)。Qt 的 `toPlainText()` 会转换不换行空格等字符，因此试验读取 `toRawText()` 并显式处理内部段落分隔符，见 [Qt 6.8 QTextDocument](https://doc.qt.io/qt-6.8/qtextdocument.html#toPlainText)。

## 重要限制

- 使用 `QSaveFile` 同目录暂存且关闭直接覆盖回退；两次比较目标字节能发现测试中注入的变化，但最后一次检查到提交之间仍有跨进程竞争。相同字节的文件替换、系统文件 ID、链接／重解析点、权限和网络卷尚未解决，不能用于真实用户文件保存。参见 [Qt 6.8 QSaveFile](https://doc.qt.io/qt-6.8/qsavefile.html)。
- 打开时拒绝混合换行、非 UTF-8 和源文已有的 U+2028／U+2029；编辑过程中 U+2028 拒绝保存。Qt 内部普通段落也使用 U+2029，因此编辑过程中插入字面 U+2029 尚不能和普通换行区分，这是待决的文本保真问题。
- `MarkdownNoHTML` 和占位资源加载只覆盖当前实验路径与样本；未完成对完整方言、嵌入对象、实际本地图片、链接协议、复杂文档及渲染资源上限的安全／兼容验收。
- 没有注册系统文件关联，也未验证操作系统双击、真实输入法、屏幕阅读器、原生菜单、真实桌面视觉、Linux 或 macOS。隐藏窗口创建成功不等于这些场景通过。
- 基准是单次合成样本，非真实键盘延迟、首次显示或产品性能结论；不使用共享机器的一次数字确定性能上限。

## 构建与运行

要求 Qt 6.8 或更新版本的 Core、Gui、Qml、Quick、Test 开发组件，以及 CMake、Ninja 和对应平台编译器。Qt 版本限制只适用于本试验；最终工具链尚未决定。需要能找到 Qt 动态库、平台插件和 QML 模块。

Windows 在 x64 MSVC 开发环境中执行：

```powershell
$xdnoteQtPrefix = 'C:\path\to\Qt\6.8.x\msvc2022_64'
pwsh -NoProfile -File experiments/qt-document-spike/run-windows.ps1 `
  -QtPrefix $xdnoteQtPrefix -NativeWindowTests -Benchmarks
```

若 CMake 或 Ninja 未在 PATH，可通过 `-CMakePath`、`-NinjaPath` 传入已有工具的绝对路径。脚本不会下载安装工具。更换 SDK 或编译器时传入不同的 `-BuildName`，避免复用旧 CMake 缓存。脚本在当前进程临时设置 Qt 路径和项目内的 `build/temp`，结束时恢复原环境；构建与报告位于忽略的 `build/`。MSVC 临时目录应使用该编译器能够处理的路径；本机遇到中文临时目录时使用项目内纯英文路径。

其他平台可在已配置 Qt 环境后使用常规 CMake 流程：

```sh
cmake -S experiments/qt-document-spike -B build/qt-document-spike \
  -DCMAKE_PREFIX_PATH=/path/to/qt -DCMAKE_BUILD_TYPE=Release
cmake --build build/qt-document-spike
ctest --test-dir build/qt-document-spike --output-on-failure
```

这条 CTest 路径运行离屏用例。其他平台的构建命令是可尝试的通用入口，不代表本次已经在这些平台运行。

## 证据

Windows 脚本每次创建新的 `build/qt-document-spike/runs/<随机标识>/`，保存源码 SHA-256、配置／构建日志、QtTest 报告和可选基准 JSON。源码在一次运行中发生变化会使脚本失败。自动测试文件只创建在已核验的构建目录子目录，测试只清理其独立的临时目录。

结果应结合工具链、平台、插件、样本和实际报告解读。测试数包含 QtTest 初始化和清理项；离屏、隐藏原生窗口、真实桌面与人工验收是不同范围。
