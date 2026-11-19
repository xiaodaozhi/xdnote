# Linux Orca 读屏验收

同一入口支持隔离 X11 和原生 GNOME Wayland。两种模式都启动真实 Orca，从 Orca 送给 Speech Dispatcher 的语音中取得样本标记，并用 AT-SPI 核对目标位置和关闭。优先运行脚本。Computer Use 或人工操作用于脚本覆盖不到的桌面交互。

## 准备

在 Linux x86_64 上按[开发构建](../engineering/developer-build.md)准备 Python、Rust、固定 Qt SDK 和桌面依赖，完成 `default` 配置的本机构建。Ubuntu 22.04 的读屏会话额外使用以下系统软件包。在已完成依赖准备的主机上直接运行脚本即可。

```bash
sudo apt install orca python3-pyatspi dbus-x11 xvfb xauth openbox xdotool \
  pulseaudio speech-dispatcher-espeak-ng
```

Wayland 模式还需要 GNOME Shell 及其嵌套运行依赖。

```bash
sudo apt install gnome-shell gnome-settings-daemon xwayland
```

用普通用户从仓库根目录运行。脚本新建 Xvfb 和 D-Bus 会话，以及临时 XDG 配置、运行目录和 PulseAudio 空输出。它通过 `desktop.py run` 打开自建 Markdown，键盘事件只进入独立测试显示。以下示例中的构建目录和 Qt 前缀可替换为本项目实际的配置。

## X11

```bash
bash scripts/reader-acceptance/orca-linux.sh x11 \
  build/desktop-admission build/Qt/6.8.2/gcc_64
```

xdnote 使用 `QT_QPA_PLATFORM=xcb` 直接连接独立 Xvfb，由 Openbox 管理窗口。`backend.txt` 记录 `xdotool` 找到的样本 X11 窗口及几何。此模式覆盖 X11 阅读交互。

## 原生 GNOME Wayland

```bash
bash scripts/reader-acceptance/orca-linux.sh wayland \
  build/desktop-admission build/Qt/6.8.2/gcc_64
```

GNOME Shell 在独立 Xvfb 中运行嵌套 Mutter，发布私有 Wayland 套接字。启动 xdnote 时清除 `DISPLAY`，指定 `QT_QPA_PLATFORM=wayland`。`xdotool` 只向外层合成器窗口输入。读取 `backend.txt` 中客户端的 `xdg_wm_base.get_xdg_surface` 和 `xdg_surface.get_toplevel` 记录，确认 xdnote 的顶层窗口走原生 Wayland。合成器启动日志保存在 `mutter.log`。

## 取证与判读

每次运行在 `build/reader-acceptance/orca-x11/` 或 `build/reader-acceptance/orca-wayland/` 下新建目录。脚本退出码为 `0` 且该次目录下有 `result.txt` 中的 `PASS` 才记为通过，两种显示协议分别记录。

| 文件 | 取证内容 |
| --- | --- |
| `environment.txt`、`build-identity.json` | Linux 发行版、架构、Git 提交、Orca 版本、Qt 与构建身份。Wayland 另记录 GNOME Shell 版本和套接字 |
| `backend.txt` | X11 样本窗口或 Wayland 客户端建立顶层窗口的协议证据 |
| `speech-markers.txt` | 正文、选区、Tab 链接播报，以及激活链接后执行「全部朗读」所得的目标段落标记 |
| `atspi.txt` | 目标段落由视口外进入视口，关闭后样本窗口从 AT-SPI 树消失 |
| `fixture-before.sha256`、`fixture-after.sha256` | 样本原文前后摘要，须一致 |
| `run.log`、`orca.log`、`app.log` | 失败步骤、Orca 原始输出及应用诊断。公开汇报优先摘录上述精简证据 |

真实读屏用 Orca 的桌面键位。脚本用数字键盘 `+` 连续朗读，用 `Insert+Shift+↑` 朗读当前选区。链接从 Tab 焦点激活，目标在视口内后再触发连续朗读。AT-SPI 提供位置和生命周期证据，语音判定来自 Orca 的 Speech Dispatcher 输出。

支持独立运行，也可手动触发 [Desktop admission](../../.github/workflows/desktop-admission.yml) 的 `reader-orca` 范围。该范围在 Ubuntu 22.04 构建一次，依次运行 X11 与 Wayland 并保留各自证据。合成器、音频或 AT-SPI 会话未就绪时记录为环境问题。运行至读屏步骤后缺少预期标记时，按失败阶段复现并核对日志。

本脚本覆盖自建样本及其隔离图形会话。真实桌面外设和图形栈验收按[平台目标](../engineering/platform-support.md)单独记录。
