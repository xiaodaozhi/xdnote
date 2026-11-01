# 桌面工程与本地构建

`apps/desktop` 和 `crates/markdown` 是进入正式工程目录的第一批基础代码。当前可运行目标为 `xdnote-admission`，只操作内存样本文档，用于验证同一窗口中的 QWidget、QML、Scintilla 和 Qt WebEngine。它不是主工作区或独立文件窗口的视觉交付，不接收文件参数、不读写用户文档，也不提供安装包。

## 工具与依赖

[依赖清单](../../tools/desktop-dependencies.json)、[Cargo.lock](../../Cargo.lock)和 [rust-toolchain.toml](../../rust-toolchain.toml)锁定本阶段输入。Qt 6.11.3、Scintilla 5.6.7、pulldown-cmark 0.13.6 与 Rust 1.99.0 构成本阶段组合；旧试验保持各自版本，不混用 SDK。Qt 采用动态库，Scintilla 从公开 Qt 端口源码构建，Rust 服务为静态库。编译器以 Qt 当前[支持矩阵](https://doc.qt.io/qt-6.11/supported-platforms.html)为准：Windows CI 使用 MSVC 2022，macOS 按主机分别构建 x86_64／arm64，Linux 最低参考构建主机使用 Ubuntu 22.04 x86_64。宿主构建通过不能替代最低系统运行验收。

需要 Python 3.12、CMake 4.4.4、Ninja 1.13.0、Rust 1.99.0（含 rustfmt、clippy）及本机 C++ 工具链。下列准备命令允许联网；普通构建和测试不下载 Qt、源码或 Cargo 依赖。工具可安装在独立虚拟环境，不修改全局 Rust 默认版本。Windows 在 x64 开发者终端运行，或给入口提供 `--vcvars`，指向已安装的 `vcvars64.bat`。Linux 还需 Qt WebEngine 所用桌面运行库及图形环境，具体 CI 安装列表见[工作流](../../.github/workflows/desktop-admission.yml)。

```text
python -m pip install aqtinstall==3.3.0 py7zr==1.0.0 cmake==4.4.4 ninja==1.13.0
rustup toolchain install 1.99.0 --profile minimal --component rustfmt --component clippy
```

## 分阶段入口

先准备 Qt，按当前系统选择一行。SDK 进入忽略的 `build/Qt/`；安装脚本保留 aqt 的元数据及二进制摘要校验。Windows 6.11.3 按编译器分目录的适配限定在该固定版本，不使用忽略摘要选项。

```text
python scripts/prepare_qt.py --download --host windows --arch win64_msvc2022_64
python scripts/prepare_qt.py --download --host mac --arch clang_64
python scripts/prepare_qt.py --download --host linux --arch linux_gcc_64
```

以下入口三平台共用。`prepare` 显式下载并核验固定 Scintilla 源码及 Cargo 锁定依赖；Scintilla 准备器复用[现有脚本](../../experiments/scintilla-buffer-spike/prepare.py)，不会将内部缓冲区试验实现带入桌面工程。重复准备沿用已存在目录，不覆盖本地修改；需要全新核验时选用干净工作区。`build`、`test` 均离线使用已准备的依赖。

```text
python scripts/desktop.py prepare --download
python scripts/desktop.py doctor
python scripts/desktop.py build
python scripts/desktop.py test
python scripts/desktop.py run
```

可用 `--qt-prefix` 指定完整的同版本 SDK，`--cmake`、`--ninja` 指定已安装工具，`--build-dir` 指定本仓库 `build/` 下的专用目录。入口拒绝逃逸路径和链接目录。构建记录源文件摘要、平台、架构与依赖清单，测试前拒绝使用内容已变化的旧构建；报告与窗口自身截图只进入忽略的构建目录或有期限的 CI 工件。测试日志中的 `QCRITICAL`、`QFATAL` 与断言失败即使未导致测试进程失败，也视为失败。

## 当前模块边界

- `DocumentSession` 在界面线程持有唯一 Scintilla 文档与撤销栈；隐藏的公开控件维持文档寿命，可见视图通过公开文档引用接口接管。快照按显式字节长度读取，保留原文中的中文、补充字符、空字符及不同换行字符。编辑命令校验 UTF-16 边界、预期版本、只读、组合输入、关闭与重入。
- `SourceEditor` 提供控件输入与基础 Qt 辅助功能文本接口；辅助功能写入使用可撤销会话命令，拒绝原因对界面与辅助功能描述可见。Qt 的无返回值 setter 尚不能完成原生失败状态映射，原生 UIA／屏幕阅读器准入继续跟踪。
- `MarkdownPreview` 只把不可变 UTF-8 快照交给 Rust，单个解析任务配合最新待处理快照，过期代次不能成为展示完成结果。FFI 输出由 Rust 分配并释放，不跨线程传递控件或文档指针。解析期间的强制取消、时间／内存预算与独立进程隔离尚未完成。
- 本阶段预览转义原始 HTML，图片和链接只有惰性文字占位，不生成文档控制的资源或导航 URL；WebEngine 禁用脚本、文件访问和远端访问，页面策略与请求拦截再次限制资源。尚未准入图片授权、工作区链接和外部导航，不将占位行为当作完整产品语义。
- `AdmissionWindow` 采用 QWidget 容器，QQuickWidget 承载共享操作，Scintilla 与 WebEngine 保持各自公开 QWidget 端口。共享控件从同一视觉参数读取尺寸、字体、颜色与 Tab 焦点外框；此窗口只验证承载，原生装饰和主界面复刻另行验收。

运行器与测试共用图形和交互初始化。Windows 选择 D3D11，CI 显式偏好系统 WARP 软件适配器；Linux／macOS 当前使用 OpenGL。图形初始化发生在应用对象建立前，同一顶层窗口共享图形 API。应用对象建立后设置本应用的 `TabFocusAllControls` 策略，使按钮也可参与 Tab 循环，不修改系统偏好。Linux CI 安装 Noto CJK 作为镜像的系统字体，应用仍使用系统字体及回退，不随此工程分发该字体；测试检查常规与等宽字体的中文成形不存在缺字。不得以跳过截图、隐藏图形错误或只检查 QML 加载状态代替渲染验证；硬件 GPU、远程会话和其他图形后端仍须分别验证。

2 MiB 源文与 8 MiB HTML 是本阶段解析额度，页面传输另受 `setHtml` 的 2 MiB 编码后大小限制，超出时明确返回预览错误。这些不是产品长文承诺；控件原生输入可能暂时超过快照额度，此时拒绝生成快照，撤销仍可恢复。正式容量、延迟及拒绝交互必须在接入用户文件前确定。真实输入法、屏幕阅读器、混合 DPI 物理拖动、完整明暗主题与安装后运行继续按[架构准入](architecture.md)、[平台目标](platform-support.md)和[窗口几何](window-geometry.md)验证。
