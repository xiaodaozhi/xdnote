# 产研与 GitHub 协作流程

## 任务来源与分流

需求、Bug、视觉设计和技术研究统一在 GitHub Issues 跟踪；先搜索已有任务，避免重复。Issue 记录问题或场景、范围、可验证的验收、关联规格、依赖与阻塞。较大工作可由父任务关联可独立验收的子任务。没有真实缺陷证据时使用研究任务，不虚构 Bug。

每个 Issue 选择一个 `type:`，分流时选择一个 `priority:` 和一个 `status:`，必要时可有多个 `area:`。优先级 P0 表示阻塞关键可行性或数据安全，P1 表示当前阶段核心能力，P2 表示不阻塞当前阶段的改进。状态依次使用 `needs-triage`、`ready`、`in-progress` 或 `blocked`；阻塞时记录原因和恢复条件。Milestone 对应 [D0–D5 阶段](../planning/roadmap.md)，不把里程碑当作发布日期承诺。

产品／视觉文档只维护当前规则与决策；单次进度、PR、实际检查和结论记录在 Issue、PR 或 CI 运行中。研发决策写清备选、依据与限制，不把参考项目的技术路线直接当作本产品的决定。

## 验收证据复用与去重

开始或继续一项工作前，先核对相关 Issue 正文与评论、关联 PR／CI 及仍保留的原始报告，列清已完成范围和实际缺口。对照验收目标、测试环境、被测提交及配置，确认已有结果是否适用于当前问题；涉及代码、配置或测试输入变更时，检查变更是否影响该结果。已有结果只按原提交、环境和范围引用，不改写为当前提交的新运行结果。

已经完成且仍适用的场景直接复用其证据，不重复执行，也不再次要求用户操作。只对缺失、失败、证据不适用或受相关变更影响的部分补验，并记录需要补验的具体差异和原因；当前 head 必需的检查仍按适用门槛执行。解锁桌面、重新接续会话或切换执行者本身不是重复验收的理由。

优先使用能在目标环境取得所需证据的脚本，只有脚本无法覆盖时才安排 Computer Use 或人工操作。脚本、真实输入法／辅助技术、物理拖动与人工视觉分别核对，不能互相冒充；证据缺失或不适用时明确记录，不因避免重复而宣布通过。

## 实施、评审与合并

1. 范围和验收足够清晰后，从最新 `main` 创建带 Issue 编号的短期分支，开始实施时更新状态。
2. 一个 PR 保持可审查范围；说明行为变化、受影响的规范或设计、测试环境及实际结果。设计图先展示并按要求取得验收，不从图片推断应用能力。
3. 按风险执行适用验证，分别记录文档／素材检查、技术试验、产品行为、真实平台与人工视觉。失败、缺失、取消或旧提交的 CI 不算当前 head 通过。
4. 当前 head 检查、必要审查和本次范围验收均完成后，合并至 `main`。全部 Issue 验收满足才用 `Closes` 或关闭；部分完成使用 `Refs` 并保留未完成项。
5. 核对合并提交及 Issue 状态，只清理由本次工作创建且已合并的临时分支／临时数据；有未合并工作或不确定归属时保留。工作区允许时快进同步本地 `main`。

已授权的开发与修复默认包括推送任务分支、创建 PR 及通过门槛后的合并，无需逐步提醒；用户指定草稿、只提交、暂不合并或对设计提出修改时遵守指定边界。单维护者可以记录自审，但不能把自审冒充独立审查。原生或人工验收有待外部条件时，Issue 继续开放并明确原因。

用户询问已授权事项是否完成、还有哪些遗留或接下来如何处理时，先核对实际状态，发现具备条件的未完成工作就在本轮继续实施、验证和收尾。遇到需要用户决定、额外授权或外部条件的阻塞时，先完成其他可推进的部分，再报告具体阻塞和恢复条件。用户明确要求只查状态、只给建议或暂停时，按指定范围处理。

## 日常 CI 与本地验证

[CI](../../.github/workflows/materials.yml) 在 PR、main 推送及手动触发时运行一个 Linux 作业，检查材料、链接、JSON、图片身份、参考切片、仓库卫生，以及工作流触发和构建工具安全契约。旧 PR 运行可取消，失败或取消不算通过。它不构建桌面产品，不执行 Qt 或 Rust 业务测试；以当前提交的实际运行结果为准。

提交和推送前，按改动风险执行本地验证；改变影响结果的源码或配置后重新验证。文档与设计检查链接、JSON、图片引用及材料摘要；Rust、C++ 和 Qt 改动构建受影响目标并运行相关测试；文件保存、会话与跨入口路由变化覆盖失败和冲突路径；窗口改动同时检查共享参数、交互和实际视觉。可用入口见[开发构建](developer-build.md)。工具或环境缺失时记录未执行项，不把其他机器或旧提交的结果记为本次通过，也不要求安装 Git 钩子。

## 按需跨平台检查

[Desktop admission](../../.github/workflows/desktop-admission.yml)、[Qt 文档试验](../../.github/workflows/qt-document-spike.yml)、[Scintilla 试验](../../.github/workflows/scintilla-buffer-spike.yml)和 [Markdown 解析试验](../../.github/workflows/pulldown-markdown-spike.yml)均通过 `workflow_dispatch` 手动运行。根据改动影响和验收需要选择，记录运行地址、提交及实际结果；日常 CI 通过不代表这些检查通过。桌面工作流保留 Windows、Linux、macOS ARM64及 Windows v142 兼容配置，汇总检查要求全部矩阵成功；缺失、失败、取消或跳过不能记为成功。固定 Qt 6.8.2 与构建细节见[开发构建](developer-build.md)。

桌面测试覆盖组件准入、真实临时文件操作、独立窗口、几何与工作区会话等路径，诊断工件保留七天。各技术试验验证的范围及限制见其 README；试验通过不等于产品准入。Linux 使用虚拟显示，自动化、合成输入、窗口截图及 CI 宿主均不能代替真实输入法、读屏、文件管理器、物理多屏或人工视觉验收。按风险和可用环境执行原生检查，记录未验证项，不要求每次执行固定人工清单。

桌面工作流手动选择 `scope=workspace-linux` 时，仅运行单套 Linux 的文件、引用扫描与链接导航定向回归及 Rust 检查；运行名称和汇总检查会标明范围，不运行完整桌面矩阵。`scope=resources-unix` 仅运行 Linux、macOS ARM64 的受影响文件、图片、引用路径维护定向测试，不执行完整桌面用例或 Windows 兼容配置；本机可用 `test --suite resources` 复现同一范围。`scope=resources-macos` 仅复验 macOS ARM64的相同资源用例，用于 Darwin 专属修正，不重复已通过的 Linux；`scope=resources-linux` 仅补验 Linux 的同一资源范围，用于 Linux 专属修正。资源定向失败保留该范围的报告，不自动再用调试器执行全部工作区用例；既有崩溃诊断仅用于 `workspace-linux` 专项。`scope=preview-unix` 只运行 Linux 和 macOS ARM64的原生预览／实际产品定向回归，使用 `test --suite preview`。`scope=background-unix` 只运行 Linux、macOS ARM64的[固定离屏回归与性能检查](../qa/background-validation.md)，使用 `test --suite background`，不依赖解锁桌面。`scope=full` 保留完整矩阵。普通提交、合并或仅同步分支不构成重复运行完整桌面 CI 的理由；以影响路径的本机验证及必要平台补充为准，不将定向结果记为全平台通过。

CI 使用只读仓库权限和固定依赖，不签名或发布。普通开发安装验证只在 `build/` 的专用目录中操作自有部署副本和自建数据样本；Windows 原生安装专项在一次性 CI 账户中使用专用临时安装目录，允许创建并核对应用拥有的候选关联和快捷方式，不更改默认应用或 UserChoice。专项不得在个人账户中运行。产品最低系统目标独立于构建主机，见[平台目标](platform-support.md)。设计原稿、提示词、可编辑素材和试验源码保留；导出 ZIP、运行截图、报告、缓存及各层级 `dist/` 不进入源码。进度和单次验证结果记录在 Issue 或 PR。

`scope=ime-linux` 在一次性 Ubuntu 22.04 上分别用 X11 和 GNOME Wayland 图形会话运行真实 IBus 智能拼音。Wayland 通过 GNOME 输入源快捷键切换引擎，验收用 `zwp_text_input_v3` 的预编辑和提交事件确认原生输入路径。两套脚本用自建 Markdown 样本核对候选与提交、独立窗口移交工作区、选区、撤销重做及磁盘摘要，并通过独立 AT-SPI 客户端检查整段写入与撤销。私有 IBus 和 Wayland 协议日志随 CI 工件保留。

`scope=scroll-unix` 仅在 Linux、macOS ARM64 执行[双栏段落映射](scroll-synchronization.md)与受影响的预览／接管离屏回归，使用 `test --suite scroll`；不包含完整桌面矩阵或物理滚动验收。

`scope=ui-unix` 仅补验 Linux、macOS ARM64 的原生阅读与共享界面回归，使用 `test --suite ui` 的固定离屏环境；Windows 原生系统命令与动画设置以独立本机证据记录。界面截图与几何回归不替代用户要求的视觉验收，也不因定向范围新增而重复完整桌面 CI。

`scope=settings-unix` 仅补验 Linux、macOS ARM64 的[设置、偏好及自动保存](settings-window.md)受影响路径，使用 `test --suite settings` 固定离屏运行；不执行真实文件选择器、原生辅助技术或完整桌面矩阵。新设置窗口的人工视觉验收另行记录，未验收时不宣称通过。

`scope=deployment-unix` 仅在 Linux、macOS ARM64 执行[纯产品部署](deployment.md)：独立收集实际应用与资源助手依赖，拒绝测试／Quick／QML 内容，封存文件、逐项 SDK／SPDX 和代码节证据；由目录外的无 Qt Test 探针验证明暗阅读、图片、编辑撤销保存和会话接管，再在 `build/` 中完成[安装／升级／卸载与文件保护](user-installation.md)，保留证据而不发布软件。`scope=deployment-linux` 复用同一 Linux 开发包，在一次性 GitHub 托管 Ubuntu 22.04 运行器中通过 apt 安装并以 dpkg 核对版本、文件归属与卸载。GIO 在私有 XDG 配置中验证原生启动器及显式、默认入口转交。另起私有 X11、D-Bus 和 XDG 会话，驱动实际 Nautilus 的「打开方式」「属性 → 打开方式 → 设为默认」和双击，分别核对一次性打开、默认应用持久化、运行中转交与冷启动。另在嵌套 GNOME Shell 和私有 Wayland 套接字中，通过文件属性选择默认应用，再用 Nautilus 文件项的打开动作验证冷启动和运行中转交，并核对实际加载的 Qt Wayland 插件。两套桌面复用同一次包安装，分别保留报告；其他桌面与发行继续分项验收。

桌面工作流的 Ubuntu 22.04 依赖准备使用显式 jammy／updates／security 源与 Ubuntu 归档密钥；主站域名解析失败时改用 [Linux Kernel Archives 的 Ubuntu 镜像](https://mirrors.kernel.org/)，实际地址写入日志，签名与有效期检查保持启用。APT 配置只对该命令生效，不沿用宿主预设的镜像优先级。DNS 探测、索引更新、包下载及整个准备步骤均有时限；网络故障作为基础设施失败记录，不无限等待或计为产品测试结果。

桌面工作流选择 `scope=ipc-windows` 时，仅构建并运行 Windows AppContainer 单实例隔离专项，使用一次性容器身份检查实际产品 IPC 的预占拒绝、跨容器访问、转发和并发首启动；专项不运行完整桌面矩阵，也不代替跨用户或正式安装验收。

## 虚拟机测试

创建或启动虚拟机前，须取得用户明确许可；本机和远程环境（包括 CI 中专门启动的测试虚拟机）均适用。请求许可时说明用途、运行位置和清理方式。锁屏、自动化测试或一般开发授权本身不构成虚拟机许可；已明确授权的范围内无需重复询问。

此前的 macOS 测试使用 Tart 调用 Apple Virtualization.framework，在本机以 `--no-graphics` 和 `--vnc-experimental` 启动隔离客体，通过本地 VNC 脚本操作。Windows 11 测试使用 GitHub Actions 的 Ubuntu 运行器，通过 QEMU 和 KVM 启动 x86_64 客体，使用虚拟键盘及控制台脚本。这些是启动方式的记录，不代表相关平台验收通过，也不构成以后启动的许可。

每个目标任务完成后，及时停止该任务启动的虚拟机和相关进程，删除该任务专用的虚拟磁盘、快照、系统镜像、临时凭据及其他可丢弃资源；远程任务同时确认所创建的实例或作业已结束。保留需要的验收报告和截图，不删除其他任务或用户的数据与共享工具。

## 工具、诊断与周期收尾

[Update core](../../.github/workflows/update-core.yml) 为手动触发的单套 Ubuntu 22.04 Rust 工作区检查，覆盖无 Qt 的更新候选、清单认证和包流校验及记录的依赖材料，不启动桌面、不签名或发布。日常材料 CI 的触发与范围保持不变；此专项不代替网络、文档保护、原生更新或商店安装验收，核心边界见[更新契约](release-and-updates.md#更新核心边界与验证)。

桌面工作流的 `scope=updates` 仅在 Windows 默认工具链、Linux、macOS ARM64 执行 `test --suite updates`：固定离屏的主动检查、受控下载、文档交接及设置回归；macOS 另执行原生生命周期夹具。不运行 v142、完整桌面用例或安装／发布，也不把合成 TLS 错误或生命周期回调夹具等同于真实 HTTPS／代理或原生更新验收。构建产物与报告保留实际开发身份，按本次 head 核对结果。

`scope=updates-macos` 只补验同一更新范围的 macOS arm64，不重复已通过的 Windows／Linux。桌面工作流可显式选择 `macos_runner=macos-15`（默认）或 `macos-26`；两者均为 GitHub 提供的原生 arm64 标准宿主，实际宿主名进入作业和工件名。宿主版本选择不改变 macOS 14+ 产品目标，也不能作为最低系统验收。标签以 [GitHub runner 清单](https://github.com/actions/runner-images/blob/main/README.md)为准，不使用付费 large／xlarge 或 Intel 宿主替代原生 arm64 检查。

独立 `update-helper.yml` 仅手动触发，在一次性 Windows／Ubuntu 22.04 账户运行[原生事务夹具](../../experiments/native-update-fixture/README.md)。它使用临时信任根和独立进程映像实际执行 helper 与安装器，覆盖取消、并行门、验签／摘要拒绝、父进程等待、安装后身份和重启；不编译 Qt 桌面、不发布，也不替代可见向导、polkit 对话框或产品完整更新验收。Linux 系统包操作仅用于一次性 GitHub 托管 CI 的原生事务和 Linux 部署专项。

手动选择 `update-helper.yml` 的 `scope=polkit` 时，Ubuntu 22.04 在一次性本地虚拟控制台登录中运行系统终端认证代理。`scope=polkit-gui` 在同一个活跃席位内另启隔离的 GNOME Wayland 会话，由 GNOME Shell 显示实际授权弹窗。两种专项都用固定策略和已安装的助手，先撤回授权核对旧包，再用临时管理员通过 PAM，校验签名、等待旧进程退出、通过 apt 安装并核对重启身份。图形专项另确认掩码密码框、取消回执和重启后连接原 Wayland 套接字，截图在输入密码前采集。工件记录席位、事务回执和两版测试包摘要。正式信任与桌面应用完整更新交互另行验收。

`scope=update-products` 另调用其 product 范围，在相同的一次性环境编译两个私有产品版本，验证真实文档保存／取消、原生包升级、回执与重启。临时信任根、文件回复及静默安装／授权限定于私有 worktree；正常产品构建和信任文件不修改。该范围仍不包含最低系统、公开发行、真实授权对话框或人工桌面验收。

`scope=qt-replacement` 只调用手动 `qt-replacement.yml`：Windows、Ubuntu 22.04、macOS arm64 各构建一份实际产品和源码修改的 Qt Core，在自有安装副本中完成加载、文档行为及维护保护验证。原 SDK／产品不覆盖，不执行系统注册或发布，不上传 Qt 源码和二进制。许可声明与全部对应源码义务仍由最终交付核验决定，见[替换说明](qt-library-replacement.md)。

`scope=sparkle-macos` 只运行 macOS ARM64 的[原生更新网络准入](macos-update-transport.md)：显式准备固定 Sparkle 输入、编译主库并执行回环代理／TLS 探针，不准备 Qt 或执行桌面产品。报告与原始构建／许可回执保留七天，汇总要求所选架构成功；该范围不执行安装、生产签名、公开更新或 最低支持系统验收。

`windows-installer.yml` 手动选择 `scope=fixtures` 时，使用不可执行的自建文件验证 Windows 原生安装引擎、升级／卸载和候选关联保护；选择 `scope=product` 时，只运行一个 Windows 产品任务，构建实际载荷、核对逐文件许可材料并执行真实安装包的安装／升级／卸载和安装目录运行。同一封存安装包另在托管 Windows Server 2022 桌面通过英文图形向导安装，随后通过 Explorer 属性选择显式默认应用，并验证文件冷启动和同进程转交。工件记录向导页序、自建文档、构建身份与包摘要。客户端系统继续分项验收。`desktop-admission.yml` 的 `scope=installer-windows` 仅转调默认夹具模式，并跳过整个桌面矩阵，供新工作流首次合并前验证；日常提交和 main 合并仍仅触发材料 CI。

项目专用 SDK、虚拟环境、构建工具和缓存归本项目所有，放在 `build/`；禁止从其他项目目录执行、导入或链接工具、SDK、库及产物，也不复制其他项目的虚拟环境充作独立安装。系统安装的 Python、Rust、Node 和编译器可以使用。同项目 worktree 可共享主工作区的 `build/toolchain/` 与 Qt；共享前核对真实路径与 Git 共同目录，不能借此访问其他项目的工具。工具缺失时使用系统运行时在本项目显式准备，不跨项目回退。迁移后重新配置活动构建目录，检查 CMake、Ninja 和 Python 路径，旧缓存不能继续指向其他项目。

本地优先复用[固定的 Qt 与工具环境](developer-build.md)，先检查实际路径和版本；PATH 找不到不等于机器未安装。CI 工作流变更应同步更新触发和门槛契约测试，查看当前提交的实际运行；日志中的警告按阶段、来源和风险核对，修复可复现的构建配置或代码问题，不以全局屏蔽代替修复。离屏平台插件能力提示与产品故障分别判断；尚未重跑时只描述预期，不声称警告已出现或消失。界面静态检查修复还要验证运行时几何、文本和交互未回归。

一个研发周期开始时，为可丢弃的临时输出创建专用目录：

```text
python scripts/cleanup_cycles.py --init cycle-<标识>
```

收尾时先核对已合并分支、工作树、运行进程与需保留的证据，再预览并清理本周期的专用目录。没有记录归属、仍被使用或尚需复验的文件保留，不清理他人改动。可手动扫描遗忘的标记目录；确认结果且确定没有进程使用后才执行：

```text
python scripts/cleanup_cycles.py --path cycle-<标识>
python scripts/cleanup_cycles.py --path cycle-<标识> --apply --yes
python scripts/cleanup_cycles.py --stale-days 30
python scripts/cleanup_cycles.py --stale-days 30 --apply --yes
```

脚本只处理 `build/` 下直接创建且带标记的 `cycle-*` 目录；不接管已有目录，不处理共享 SDK、缓存或未标记的旧文件。活动 Git worktree、路径链接及内部链接或 Windows 重解析点会被拒绝；年龄扫描按目录内最新修改时间筛选。脚本不能识别所有运行进程或证据价值，执行者须先检查；不要在并发修改目录时运行，不得递归删除整个 `build/`。托管 CI 无法清理开发者本机，兜底由本机手动执行。

[更新签名工作流](../../.github/workflows/release-signing.yml)在 `main` 上使用受保护的环境，为同提交的已核验候选生成签名附件。独立验签作业重新下载该候选与附件，核对全部摘要和签名。完整发行候选生产和正式发布继续跟踪。[发行渠道与主动更新](release-and-updates.md)已确定未签名 Windows、后续 Store MSIX 及 macOS ad-hoc／无 Developer ID／无 Apple 公证的方案。将版本、源码提交、验证记录和工件绑定，同一候选在一次受控运行中尽可能复用产物。Qt 源码不得进入本项目 Release 附件、安装包或产品归档，源码获取与替换要求按[分发边界](distribution-audit.md)独立落实；正式发布仍需完成适用材料与运行验收并取得具体授权。

桌面工作流的 `scope=release-candidate` 使用显式 `candidate_identity` 和 `candidate_recipient`。身份输入复用 `scripts/desktop.py build --release-identity` 的 JSON 格式，指定版本、build、上一版和 `main` 历史中的完整源码提交。收件人输入使用临时 age 密钥的公钥。Windows x64、Ubuntu 22.04 x64 和 macOS arm64 各生成普通渠道的两件候选包，连同材料与核验记录加密后才上传。下载到本机、解密并用 `scripts/assemble_release_candidate.py` 复核六件套。签名、真实更新与公开发行各按其适用流程核验。

`scope=tools-unix` 只在 Linux、macOS ARM64 执行文档查找搜索、恢复副本、快捷键与受影响设置／文件的固定离屏验证，使用 `test --suite tools`。它不执行真实系统关联、不安装用户目录，也不扩大为完整桌面矩阵；工作流仍只允许手动触发。

`scope=tools-macos` 仅补验相同文档工具范围的 macOS ARM64，用于 Darwin 路径或平台差异修复；已有有效 Linux 产品结果不因 macOS 补验重复构建。两种 tools 范围均保留 Python 安装维护回归，不执行真实系统默认应用变更。

`scope=delivery-review` 仅执行[实际分发材料追溯](distribution-audit.md)：显式 `evidence_run` 指定同仓库成功的 Qt 替换源运行，核对其原始工件与固定 SDK。它记录来源产品和审阅工具各自的提交，不重复产品构建或 Core 源码重建，不把材料审阅清单计作新提交的产品运行验收。仓库权限限于 contents／actions 只读。
