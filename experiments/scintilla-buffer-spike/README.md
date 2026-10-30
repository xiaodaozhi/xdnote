# D1：Scintilla 文本缓冲区试验

本目录只比较文本存储候选，不是 xdnote 产品，也不决定采用 Scintilla。实验直接编译 Scintilla 5.6.7 的内部 `CellBuffer`，检查 UTF-8 字节、普通换行与字面 U+2029 的区别，以及插入／删除的撤销重做。样本包括中文、表情、U+2028、U+2029、非断行空格、NUL、LF 和 CRLF；测试不读写用户文档。

这不是 GUI 控件测试：未验证 Qt Quick 集成、中文输入法、选区、跨窗口接管、字体、双向文本、可访问性或保存。内部 API 和 UTF-8 字节下标不等同于最终产品接口或 Qt 的 UTF-16 下标；候选需要更多证据。Scintilla 的 Qt 端是 QWidget 控件，Qt 6 构建还使用 Core5Compat；与现有 Qt Quick 候选的结合方式尚未确定。

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

三平台 CI 运行的是独立核心缓冲区测试，不代表原生桌面或产品验收。[Issue #3](https://github.com/xiaodaozhi/xdnote/issues/3) 跟踪文档会话，[Issue #9](https://github.com/xiaodaozhi/xdnote/issues/9) 跟踪文件保真与保存。
