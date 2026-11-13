# 用户级开发安装

`scripts/user_install.py` 接收已经准备好实际运行依赖的部署目录，生成带所有权标记及 SHA-256 文件清单的用户级开发安装。它不是签名安装器或正式发行，也不下载运行库。Windows 输入使用[部署诊断](deployment.md)准备的目录，Linux 输入保留 `bin`／`lib`／`plugins` 布局，macOS 输入保留 `xdnote.app`。内部相对链接须留在安装根内，目录连接和越界链接被拒绝。

默认只输出操作计划；`--apply` 才写入。以下占位路径须换成实际部署目录和专用安装目录，不应指向源码或文档目录：

```text
python scripts/user_install.py install --payload <部署目录> --destination <专用安装目录>
python scripts/user_install.py install --payload <部署目录> --destination <专用安装目录> --apply --register
python scripts/user_install.py uninstall --destination <专用安装目录>
python scripts/user_install.py uninstall --destination <专用安装目录> --apply --register
```

再次安装到相同目录执行升级。工具先校验已有清单、全部文件摘要和归属；出现未知文件或被修改的运行库时保留现场并拒绝替换或卸载。升级先在相邻专用目录验证副本，再切换目录，切换失败恢复原目录；运行中程序锁定文件时应先正常退出后重试。卸载只删除核验后的安装内容，不处理用户配置、恢复记录、Markdown 或工作区。不要在安装目录中存放用户文档。

Windows 的 `--register` 只写当前用户的 ProgID、`OpenWithProgids`、Capabilities 和 RegisteredApplications；不写扩展名默认值或 UserChoice。卸载前核对当前命令仍归属于此安装，保留其他副本的注册。设置页也可显式注册当前运行副本，并打开系统默认应用页；注册到可丢弃构建目录后，清理目录会使该入口失效，应优先使用稳定安装副本。这些行为遵循 [Microsoft Default Programs](https://learn.microsoft.com/en-us/windows/win32/shell/default-programs) 和[默认应用设置入口](https://learn.microsoft.com/en-us/windows/apps/develop/launch/launch-default-apps-settings)。

Linux 生成当前用户的 `.desktop` 入口，`Exec` 保留文件参数；不改写 `mimeapps.list` 或调用设置默认值的命令，已存在的不同副本入口予以保留。依据 [Desktop Entry Specification](https://specifications.freedesktop.org/desktop-entry/latest-single/)。macOS 部署包在 `Info.plist` 声明 Markdown 编辑角色与 `Alternate` 等级，用户通过 Finder 选择默认打开方式，依据 [CFBundleDocumentTypes](https://developer.apple.com/documentation/bundleresources/information-property-list/cfbundledocumenttypes)。

当前实现是开发者可复现的安装维护入口，仍依赖 Python；尚未提供双击式安装器、发布者签名、公证、商店安装和自动更新。最终包逐项许可、最低系统和真实系统关联核验仍由独立任务跟踪，不能把隔离目录测试或声明文件生成计为三平台安装验收。
