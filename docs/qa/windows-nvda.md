# Windows NVDA 实机验收

在已解锁的原生 Windows 11 x64 桌面，以 PowerShell 7.4 或更新版本运行。脚本使用真实 NVDA，通过键盘和指针操作自建长 Markdown，读取 NVDA 送给语音合成器的样本标记。

## 准备并运行

1. 按 [Windows 图形开发安装器](../engineering/user-installation.md#windows-图形开发安装器)构建独立产品载荷，记录源码提交和载荷清单。也可复用已验明来源及 SHA-256 的开发验收包，提取其中包含 `xdnote.exe`、`qt.conf`、Qt DLL 和 `platforms/qwindows.dll` 的应用目录。脚本直接使用该独立部署目录。本机构建目录继续使用 [desktop.py run](../engineering/developer-build.md) 启动。
2. 从 [NV Access](https://www.nvaccess.org/download/) 获取 NVDA。核对发布方的 SHA-256 和签名，在项目 `build/` 下创建便携副本。可在安装器中选择创建便携副本，也可使用 `--create-portable-silent --portable-path <新的绝对目录>`。已通过的版本为 NVDA 2026.3.1。
3. 退出已有的 xdnote 和 NVDA。建议在验收账户以普通用户运行，xdnote 与 NVDA 使用同一桌面和权限级别。在仓库根目录执行下方命令。

```powershell
pwsh -NoProfile -File scripts/reader-acceptance/nvda-windows.ps1 `
  -PayloadDir build/product-install/payload `
  -NvdaExe build/reader-acceptance/nvda-portable/nvda.exe
```

将示例路径替换为实际载荷和便携副本。出现前台提示时，切到对应的 `NVDA reader check` 窗口并保持前台直至脚本结束。默认等待 60 秒，可用 `-ForegroundTimeoutSeconds` 调整。验收期间避免切换窗口、移动指针或输入。窗口移到后台时，脚本停止发送输入并清理本次启动的进程。

## 核对结果

通过启动前检查后，每次运行在 `build/reader-acceptance/windows-nvda/` 下生成独立目录。退出码为 `0` 且 `results.json` 的 `passed` 为 `true` 才记为通过。

| 证据 | 用途 |
| --- | --- |
| `results.json` | Windows 版本和架构、xdnote 构建身份与可执行文件 SHA-256、Qt 和 NVDA 版本、逐项断言及样本前后 SHA-256 |
| `speech-markers.txt` | 实际合成器输入中匹配到的固定样本标记及触发阶段 |
| `nvda-startup.log` | 核对 NVDA 启动和初始化，在本机排查加载失败、权限或超时 |
| `NVDA reader check … .md` | 脚本生成的长文档样本 |

正文检查需要出现 `body`。显式朗读选区需要在同一条合成器输入中出现 `opening, selected`。Tab 阶段需要出现 `link`。激活该链接后，「全部朗读」的首条相关输出须从 `target` 开始，并在此阶段读到 `continuation`。编辑模式需要播报 `source`，返回阅读后需再次读出正文。最后核对应用正常退出、NVDA 退出及文件摘要一致。

在 Issue 或 PR 记录被验提交、包的来源和摘要、环境与结果。共享证据时优先使用 `results.json` 和 `speech-markers.txt`。启动日志可能包含本机路径，发送前只摘录相关条目。脚本使用独立的 NVDA 配置和临时语音观察器，关闭全量输入输出日志。UI Automation 用于定位控件，读屏结论取自 NVDA 的实际合成器输入。

前台超时、程序未启动或观察器未加载均按环境问题记录。应用成功启动后缺少预期标记时，核对失败阶段与启动日志，再复现并判断产品行为。此脚本覆盖指定载荷在 Windows 11 x64 原生桌面的路径。其他系统、架构、读屏器或脚本覆盖不到的交互按对应方式补验。
