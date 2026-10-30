# D1：Scintilla 文本缓冲区试验

本目录只比较文本存储候选，不是 xdnote 产品，也不决定采用 Scintilla。实验直接编译 Scintilla 5.6.7 的内部 `CellBuffer`，检查 UTF-8 字节、普通换行与字面 U+2029 的区别，以及插入／删除的撤销重做。样本包括中文、表情、U+2028、U+2029、非断行空格、NUL、LF 和 CRLF；测试不读写用户文档。

核心缓冲区测试不验证 GUI、输入法或保存。可选的 Qt 控件试验另外检查共享文档、反向选区、撤销重做和合成输入法组合；辅助功能对照试验检查 Qt 接口暴露的测试文字。它们不是 Qt Quick 集成、真实输入法、字体、双向文本、屏幕阅读器或保存验收。内部 API 和 UTF-8 字节下标不等同于最终产品接口或 Qt 的 UTF-16 下标；候选需要更多证据。Scintilla 的 Qt 端是 QWidget 控件，Qt 6 构建还使用 Core5Compat；与现有 Qt Quick 候选的结合方式尚未确定。

## 显式准备源码

使用 Python 3.9 或更新版本。下面的命令会**明确下载** Scintilla/SciTE 5.6.7 官方源码包，核对固定 SHA-256，仅提取 Scintilla 源码至忽略的 `build/scintilla-5.6.7/`；构建本身不会下载依赖。已有目录不会被覆盖。

```sh
python experiments/scintilla-buffer-spike/prepare.py --download
```

也可将 `--download` 换成 `--archive /path/to/scite567.tgz`，使用已取得且摘要一致的源码包。固定输入为 [scite567.tgz](https://www.scintilla.org/scite567.tgz)，SHA-256 为 `ad31f6e89aefe9c4e2c48f64c720f311e6a43369288e7b109d90b507ca623198`。摘要用于在准备时固定归档输入，不自动检测提取后的改动；上游源码及其 [License.txt](https://www.scintilla.org/License.txt) 不纳入版本管理，发行依赖和授权义务仍需单独核验。

## 构建与运行

在配置好 C++17 编译器与 CMake 后执行：

```sh
cmake -S experiments/scintilla-buffer-spike -B build/scintilla-buffer-spike -DCMAKE_BUILD_TYPE=Release
cmake --build build/scintilla-buffer-spike --config Release
ctest --test-dir build/scintilla-buffer-spike -C Release --output-on-failure --no-tests=error
```

可通过 `-DSCINTILLA_SOURCE_DIR=/absolute/path/to/scintilla` 使用另一个已准备的 5.6.7 源码目录；准备命令只核验提取时的归档，构建前仍应确保源码未被修改。Windows 使用 MSVC 开发环境与匹配的 CMake 生成器；若本地 MSVC 临时目录或试编译配置有限制，应设置可写的项目内临时目录并使用 Release 试编译。

## Qt 控件与真实桌面试验

额外需要 Qt 6.8 或更新版本的 Core、Gui、Widgets、Core5Compat 组件；它们只属于本试验工具链。使用包含这些组件的 Qt 前缀配置：

```sh
cmake -S experiments/scintilla-buffer-spike -B build/scintilla-widget-spike \
  -DCMAKE_PREFIX_PATH=/path/to/qt -DCMAKE_BUILD_TYPE=Release -DXDNOTE_SCINTILLA_WIDGET=ON
cmake --build build/scintilla-widget-spike --config Release
ctest --test-dir build/scintilla-widget-spike -C Release --output-on-failure --no-tests=error
```

`scintilla_widget_spike` 使用隐藏控件和合成 `QInputMethodEvent`，检查两个视图共享文档、选区、撤销重做，以及组合期间拒绝接管。试验观察到组合文字暂时进入共享缓冲区，因此产品方案必须验证如何阻止暂存文字被保存；仅依赖该试验中的状态判断不足以证明所有输入法安全。只读标记也属于共享文档；手动工具通过禁用非活动控件限制当前操作，不表示最终多窗口设计已经确定。

`scintilla_accessibility_spike` 在离屏环境遍历当前控件的 Qt 辅助功能对象树，并以 `QPlainTextEdit` 验证测试环境能够读取对照文字。当前固定版本的 Scintilla Qt 控件未通过被遍历节点的文本接口或值暴露样本文字；该负面观察只针对本试验控件和 Qt 接口，不能证明其他封装、平台桥接或屏幕阅读器的全部表现。未来若更换候选或添加辅助功能适配，应重新评估并更新这个对照；不能把其通过视为辅助功能验收。

完成构建后，在已配置 Qt 动态库和平台插件的真实桌面环境启动 `scintilla_widget_manual`；单配置生成器的程序位于 `build/scintilla-widget-spike/`，多配置生成器在 `build/scintilla-widget-spike/Release/`，Windows 可执行文件带 `.exe`。工具只使用内存测试文字，不读取或保存文件，不要输入需要保留的内容。

在每个实际系统记录版本、Qt 版本、窗口系统、输入法名称与结果：

1. 输入并提交中文，正向和反向选择文本，用 F8 或按钮接管；检查目标文字、选区、光标以及撤销／重做，再接管回来。界面所报位置为 UTF-8 字节位置。
2. 开始中文输入但不提交，观察“输入法临时组合：是”，尝试接管；只有请求到达时仍显示组合中，才能观察拒绝。如果输入法拦截 F8 或点击先结束组合，应记录为该场景未验证。
3. 提交或取消组合后重试，检查文本和撤销栈；不要把隐藏窗口、合成事件或一次设备结果当作全部平台输入法的验收。

三平台 CI 的核心缓冲区和 Qt 控件试验均不代表真实桌面或产品验收。[Issue #3](https://github.com/xiaodaozhi/xdnote/issues/3) 跟踪文档会话，[Issue #9](https://github.com/xiaodaozhi/xdnote/issues/9) 跟踪文件保真与保存。
