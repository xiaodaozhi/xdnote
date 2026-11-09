# 桌面工程与本地构建

`apps/desktop` 和 `crates/markdown` 包含工作区／单文件功能原型与组件准入代码。默认目标 `xdnote` 可打开、阅读、编辑、保存和另存为真实 Markdown 文件，处理外部冲突及未保存关闭；使用系统外框与共享逻辑尺寸。独立窗口支持系统／浅色／深色主题、六种强调色和正文缩放；外观菜单选择通过用户级设置持久化并同步已打开窗口。无文件启动显示工作区欢迎页，可显式打开目录、使用多标签和三种视图，并接管独立窗口原会话，详见[工作区基础](workspace-foundation.md)。完整设置、资源与链接、文件关联注册、安装和发行尚未完成。`xdnote-admission` 仍为独立的内存样本窗口，只验证 QWidget、QML、Scintilla 和 Qt WebEngine 混合承载，不接收文件或读写用户文档。

## 工具与依赖

[依赖清单](../../tools/desktop-dependencies.json)、[Cargo.lock](../../Cargo.lock)和 [rust-toolchain.toml](../../rust-toolchain.toml)锁定本阶段输入。Qt 6.8.2、Scintilla 5.6.7、pulldown-cmark 0.13.6 与 Rust 1.99.0 构成本阶段组合；本机、按需桌面 CI、MSVC 2019 兼容配置和 Qt／Scintilla 试验 CI 都读取同一个 `qt.version`，不在配置中另设 Qt 版本。试验仍使用独立构建目录，不混用产物。Qt 采用动态库，Scintilla 从公开 Qt 端口源码构建，Rust 服务为静态库。默认编译器以 Qt 6.8 的[支持矩阵](https://doc.qt.io/qt-6.8/supported-platforms.html)为准：Windows CI 使用 MSVC 2022，macOS 按主机分别构建 x86_64／arm64，Linux 最低参考构建主机使用 Ubuntu 22.04 x86_64。宿主构建通过不能替代最低系统运行验收。

需要 Python 3.12、CMake 4.4.4、Ninja 1.13.0、Rust 1.99.0（含 rustfmt、clippy）及本机 C++ 工具链。下列准备命令允许联网；普通构建和测试不下载 Qt、源码或 Cargo 依赖。工具可安装在独立虚拟环境，不修改全局 Rust 默认版本。Windows 在 x64 开发者终端运行，或给入口提供 `--vcvars`，指向已安装的 `vcvars64.bat`。Linux 还需 Qt WebEngine 所用桌面运行库及图形环境，具体 CI 安装列表见[工作流](../../.github/workflows/desktop-admission.yml)。

```text
python -m pip install aqtinstall==3.3.0 py7zr==1.0.0 cmake==4.4.4 ninja==1.13.0
rustup toolchain install 1.99.0 --profile minimal --component rustfmt --component clippy
```

本机优先复用 `build/Qt/6.8.2/<平台子目录>/` 下现有 SDK，Windows 的 `qmllint.exe` 和 `qmake.exe` 位于 `msvc2022_64/bin/`；macOS 和 Linux 使用对应 SDK 的 `bin/`。工具可放在项目内固定虚拟环境 `build/toolchain/desktop-tools/`，创建及安装由开发者显式执行；不要为每个研发周期重复安装。已有环境先核对版本与实际可执行文件，CI 仍使用独立 runner。系统 Rust、MSVC 和其他宿主编译器沿用本机安装；目录约定不改变锁定版本或构建入口。

```text
python -m venv build/toolchain/desktop-tools
```

使用该虚拟环境的 Python 执行上面的 pip 安装命令；Windows 可调用 `build/toolchain/desktop-tools/Scripts/python.exe`，Unix 可调用 `build/toolchain/desktop-tools/bin/python`。研发周期临时产物的安全清理见[产研流程](workflow.md#工具诊断与周期收尾)。

## Windows 本机默认配置

本机默认使用已安装的 Visual Studio 2022／MSVC v143 x64 工具与 `default` 配置。可在 x64 开发者终端使用下列命令；非开发者终端通过 `--vcvars` 指向该安装的 `VC/Auxiliary/Build/vcvars64.bat`。本机默认输出为 `build/desktop-admission/`，Qt 使用 `build/Qt/6.8.2/msvc2022_64/`。构建日志和 `native-toolchain.json` 记录 CMake 实际识别的编译器，不以 SDK 目录名代替版本证据。

```text
python scripts/desktop.py doctor --profile default
python scripts/desktop.py build --profile default
python scripts/desktop.py test --profile default
python scripts/desktop.py run --profile default
```

## MSVC 2019 兼容配置

如需验证已安装的 MSVC 2019 v142／14.29 工具，显式选择 `--profile msvc2019`。该配置与默认配置统一使用 Qt 6.8.2，并沿用同一源码、Rust 版本及测试。两个 Windows 配置共用同一 SDK；CMake 目录和 Rust 测试产物分别保存，不修改 Qt 头文件或伪装编译器版本。

```text
python scripts/prepare_qt.py --download --host windows --arch win64_msvc2022_64 --profile msvc2019
python scripts/desktop.py prepare --download --profile msvc2019
python scripts/desktop.py doctor --profile msvc2019
python scripts/desktop.py build --profile msvc2019
python scripts/desktop.py test --profile msvc2019
python scripts/desktop.py run --profile msvc2019
```

入口优先发现已有的 2019 Build Tools，也可使用已安装 v142 工具集的 2022 宿主；两者都显式调用 `vcvars64.bat -vcvars_ver=14.29`，CMake 必须实际识别到 MSVC 19.29。可用 `--vcvars` 指定安装位置；`--vcvars-version` 只选择已经安装的工具，不负责安装。默认输出为 `build/desktop-msvc2019/`，SDK 为 `build/Qt/6.8.2/msvc2022_64/`，目录名中的 `msvc2022_64` 是上游预编译 SDK 标识，不是本机应用编译器的证明。

Qt 6.8.2 的[编译器检查](https://github.com/qt/qtbase/blob/v6.8.2/src/corelib/global/qcompilerdetection.h)允许此编译器，而[官方支持矩阵](https://doc.qt.io/qt-6.8/supported-platforms.html)仍列 MSVC 2022；[微软二进制兼容说明](https://learn.microsoft.com/en-us/cpp/porting/binary-compat-2015-2017)也对链接器、输入工具链及运行库设有限制。因此本配置只记录实际验证的本地兼容结果，不将头文件可编译等同于上游支持或任意 ABI 兼容。不得用于发行；默认工具链的四平台检查按需执行。按需 Windows CI 另用实际 v142／14.29 执行兼容回归，失败同样阻止桌面汇总通过。

构建身份记录配置、SDK 前缀、Qt 版本及 CMake 实际编译器版本。测试和运行同时核对配置、SDK 和源码摘要，不能切换参数后直接启动另一个目录中的旧产物；变更入口或源码后先重新构建。

入口合并 Windows 工具链环境时统一变量名，避免 `Path` 与 `PATH` 并存而使子进程找不到 Qt DLL。有控制台时，入口在命令期间把输入／输出代码页共同设为 UTF-8，结束后恢复，确保 CMake 对中文 `/showIncludes` 前缀的解码与 Ninja 收到的字节一致。已有配置若记录了乱码前缀，可在选定的配置下使用 `build --fresh` 重新探测；不会改变系统或应用语言。

## 分阶段入口

先准备 Qt，按当前系统选择一行。SDK 进入忽略的 `build/Qt/6.8.2/`；安装脚本使用 aqt 原有元数据布局并保留元数据及二进制摘要校验，不使用忽略摘要选项。切换版本后重新配置和构建，不在旧缓存中混入另一版 Qt。新版本通过验证且已无活动引用后，可清理本项目旧 SDK 和失效的构建产物；不清理其他项目或系统的工具链。

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
python scripts/desktop.py run --file "path/to/note.md"
python scripts/desktop.py run --admission
```

可用 `--qt-prefix` 指定完整的同版本 SDK，`--cmake`、`--ninja` 指定已安装工具，`--build-dir` 指定本仓库 `build/` 下的专用目录。入口拒绝逃逸路径和链接目录。构建记录源文件摘要、平台、架构与依赖清单，测试前拒绝使用内容已变化的旧构建；报告与窗口自身截图只进入忽略的构建目录或有期限的 CI 工件。测试日志中的 `QCRITICAL`、`QFATAL` 与断言失败即使未导致测试进程失败，也视为失败。

`--file` 可重复传入；默认配置可省略 `--profile default`，使用 v142 时须显式追加 `--profile msvc2019`。不传文件时显示选择文件入口，也支持应用内打开和拖入本地文件。后续启动经用户级 IPC 转交打开意图，失败时明确报错，不另建独立可写实例。`run` 只核对构建身份并启动已有产物，不要求在 PATH 中重新发现 CMake、Ninja 或 Rust；先完成对应配置的构建。`--admission` 与 `--file` 互斥。

## 当前模块边界

- `DocumentSession` 在界面线程持有唯一 Scintilla 文档与撤销栈；隐藏的公开控件维持文档寿命，可见视图通过公开文档引用接口接管。快照按显式字节长度读取，保留原文中的中文、补充字符、空字符及不同换行字符。编辑命令校验 UTF-16 边界、预期版本、只读、组合输入、关闭与重入。
- `FileService` 提供不依赖 GUI 的身份探测、严格读取和原位替换保存；`SessionRegistry` 管理打开登记与替换后的身份重绑定。`FileManager` 串行协调后台 I/O、冻结保存快照与 GUI 回执，编辑保持可用；`FileApplication` 与 `SingleInstance` 统一窗口、退出和跨进程打开意图。具体保证见[文件读取](file-read-baseline.md)和[保存闭环](file-save-workflow.md)。
- `SourceEditor` 提供控件输入与基础 Qt 辅助功能文本接口；辅助功能写入使用可撤销会话命令，拒绝原因对界面与辅助功能描述可见。Qt 的无返回值 setter 尚不能完成原生失败状态映射，原生 UIA／屏幕阅读器准入继续跟踪。
- `MarkdownPreview` 只把不可变 UTF-8 快照交给 Rust，单个解析任务配合最新待处理快照，过期代次不能成为展示完成结果。FFI 输出由 Rust 分配并释放，不跨线程传递控件或文档指针。解析期间的强制取消、时间／内存预算与独立进程隔离尚未完成。
- 本阶段预览转义原始 HTML。Windows 和 Linux 可在当前授权范围内读取受限本地图片，并以应用生成的内存资源提供预览；macOS 与受阻或不支持的图片仍显示占位。工作区已支持插入相对文档链接，但阅读预览中的链接仍为惰性文字占位，尚未接入链接激活或外部导航。文档不能控制资源 URL，也不能执行脚本或直接访问文件和网络；页面策略与请求拦截再次限制资源，详见[资源基础](workspace-resource-foundation.md)。
- `AdmissionWindow` 采用 QWidget 容器，QQuickWidget 承载共享操作，Scintilla 与 WebEngine 保持各自公开 QWidget 端口。共享控件从同一视觉参数读取尺寸、字体、颜色与 Tab 焦点外框；此窗口只验证承载，原生装饰和主界面复刻另行验收。

运行器与测试共用图形和交互初始化。Windows 选择 D3D11，CI 显式偏好系统 WARP 软件适配器；Linux／macOS 当前使用 OpenGL。图形初始化发生在应用对象建立前，同一顶层窗口共享图形 API。应用对象建立后设置本应用的 `TabFocusAllControls` 策略，使按钮也可参与 Tab 循环，不修改系统偏好。Linux CI 安装 Noto CJK 作为镜像的系统字体，应用仍使用系统字体及回退，不随此工程分发该字体；测试检查常规与等宽字体的中文成形不存在缺字。不得以跳过截图、隐藏图形错误或只检查 QML 加载状态代替渲染验证；硬件 GPU、远程会话和其他图形后端仍须分别验证。

首轮真实文件只接受 2 MiB 以内的严格 UTF-8 正文，编码 BOM 另计三字节，含 NUL 或无效 UTF-8 的文件明确拒绝。保留混合换行与字符，不猜测编码。预览输出上限 8 MiB，以应用私有内存页面传输，不经过 `setHtml` 的编码后数据地址限制；每页使用独立临时 profile、不可预测地址与精确导航校验，不读取磁盘或开放网络。控件原生输入可能暂时超过快照额度，此时保存／预览明确拒绝，当前内容和撤销仍保留，须缩小后重试。这是当前原型的明确容量边界，不是任意长文承诺。

`desktop_admission` 检查原有会话、辅助功能、混合承载和预览隔离；`file_workflow` 使用自身创建的真实临时文件，覆盖替换竞争、失败恢复、保存期间编辑、原生窗口按钮与关闭确认、IPC 和子进程应用路由；`standalone_visual` 检查独立窗口的十二种主题／配色组合、字形对齐、控件密度、焦点、菜单／确认框、只读反馈、阅读位置和多倍率几何；`window_geometry` 检查 DPI 算术、待处理缩放量、跟踪约束、可用区、正常偏好与临时收敛、持久化及自身窗口的 Windows 消息。`workspace_workflow` 另覆盖目录、标签、接管、组合输入延后、双栏与批量关闭；五份 QtTest 报告缺一即失败，专项范围见[独立窗口](standalone-window.md)和[窗口几何](window-geometry.md)。当前窗口截图和测量只进入构建目录中的 `evidence/`。自动化窗口操作不等于人工原生文件选择器、文件管理器关联或真实输入法验收。屏幕阅读器、混合 DPI 物理拖动与安装后运行继续按[架构准入](architecture.md)、[平台目标](platform-support.md)和[窗口几何](window-geometry.md)验证。
