# 用户级开发安装

`scripts/user_install.py` 接收已经准备好实际运行依赖的部署目录，生成带所有权标记及 SHA-256 文件清单的用户级开发安装。它不是签名安装器或正式发行，也不下载运行库。Windows 输入使用[部署诊断](deployment.md)准备的目录，Linux 输入保留 `bin`／`lib`／`plugins` 布局，macOS 输入保留 `xdnote.app`。内部相对链接须留在安装根内，安装根本身的链接／目录连接和越界内部链接被拒绝。父目录先解析为真实绝对路径，允许 macOS `/var` 等系统目录别名；归属与重叠检查使用解析后的路径，不把系统别名误判成危险目标。

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

上述入口是依赖 Python 的开发者安装工具。Windows 另提供下述图形开发安装器；发布者签名、公证、商店安装和自动更新尚未实现。最终包逐项许可、最低系统和真实系统关联核验仍由独立任务跟踪，不能把隔离目录测试或声明文件生成计为三平台安装验收。

## 独立运行目录的安装闭环

`python scripts/validate_user_install.py --build-dir <已构建目录> --payload <独立诊断目录> --destination <build 下的专用验证目录>` 保留原诊断目录的复制、隔离探针、升级与卸载验证。Unix 纯产品改用 [product_deployment_unix.py](deployment.md) 的 `--payload`／`--audit` 入口：安装与升级后分别从目录外验证明暗阅读、图片、编辑撤销保存和会话接管；修改文件或新增未知文件时维护拒绝继续，自建数据摘要保持不变。真实注册关闭，成功后移除临时安装，保留证据。手动 `deployment-unix`／`deployment-linux` 接入纯产品验证，不运行完整桌面用例或用户目录安装。

## Windows 图形开发安装器

图形安装器使用固定版本 Inno Setup 6.7.3，英文和简体中文界面，默认安装到当前用户的 `%LOCALAPPDATA%\Programs\xdnote`，不请求管理员权限。开始菜单指向稳定的安装路径，卸载入口由安装引擎登记。安装器使用原生引擎界面，不套用应用自身控件尺寸。当前输出带 `0.1.0-dev.<提交>` 开发版本，未签名，不表示正式发行或最低系统已验收。

所需编译器、中文语言文件、引擎许可、VC++ 运行库和原始协议均锁定官方地址与 SHA-256，工具配置在 `scripts/installer-toolchain.json` 和 `scripts/windows-runtime.json`。运行库仅解包，不执行其安装程序。所有项目工具放在自身 `build/toolchain/`，不加入系统 PATH。准备和构建顺序如下；命令中的占位符须换成实际目录，输出均使用本项目 `build/` 下的新目录：

```text
powershell -NoProfile -File scripts/prepare-windows-installer.ps1 -OutputDirectory <本项目 build/toolchain/installer 绝对路径>
python scripts/prepare-windows-runtime.py --output build/toolchain/windows-runtime
python scripts/deployment_inventory.py --build-dir <当前构建目录> --qt-prefix <本项目 Qt 目录> --destination <新诊断载荷目录> --redist-dir build/toolchain/windows-runtime/runtime
python scripts/installer_payload.py --source <诊断载荷目录> --audit <诊断 evidence/inventory.json> --destination <新产品载荷目录> --toolchain build/toolchain/installer --runtime build/toolchain/windows-runtime/runtime --build-dir <当前构建目录> --qt-prefix <本项目 Qt 目录> --report <新产品清单.json>
python scripts/build-windows-installer.py --root <产品载荷目录> --audit <产品清单.json> --toolchain build/toolchain/installer --output <新安装器.exe> --report <安装器报告.json>
```

产品载荷去掉诊断程序、Qt Test 和离屏平台插件，保留产品需要的 Qt 依赖与许可材料，当前不捆绑字体，图形系统组件按[Windows 基线](distribution-audit.md)由目标系统提供。精简后重新检查 PE 普通及延迟导入，封存每项大小与摘要；编译前后再次验证载荷，缺失、额外或被修改的文件均拒绝。原始 Microsoft 运行库协议在安装器许可页展示，适用范围在中英说明中区分；其他组件继续遵守各自许可，不以此替代 Qt 等组件的义务核验。

“加入打开方式候选”默认不勾选。显式选择时仅注册 `.md`、`.markdown` 的当前用户 ProgID、Capabilities、OpenWith 和 RegisteredApplications，不写扩展名默认值、UserChoice 或用户 PATH。文件命令保留带引号的路径及 `--` 分隔。卸载只撤销仍归属于此副本的注册，后来被其他副本接管的命令予以保留。

产品持有运行互斥标记，安装维护时也阻止产品启动；安装器不终止应用、不强制关闭未保存窗口。已有安装必须有合法的有界文件清单。升级前核验全部已有文件；被修改的当前／过期文件、目录连接、不合法清单、与新增文件冲突的未知文件及更新的安装格式均拒绝覆盖。首次安装拒绝非空的无归属目录。升级只清理清单内仍未修改的过期文件，失败项保留并提示。

卸载先核验清单和文件，再在确认后的卸载阶段逐项复核并删除；不会递归清理目录。未知文件和已退役路径上后来新建的用户文件保留，无法核验或被占用的文件保留并提示。用户配置、恢复记录、Markdown 和工作区不在安装清理范围内。不要将工作文档存进安装目录；清单保护不构成防恶意并发篡改的句柄级边界。

手动工作流 `windows-installer.yml` 仅在一次性 Windows CI 账户中运行真实安装引擎，使用不执行的文件夹具验证升级、卸载、Unicode 路径、快捷方式、运行占用和注册保护。它不运行完整桌面矩阵，也不把夹具安装算作产品窗口或真实默认应用验收。实际载荷可通过[整应用探针](application-performance.md)验证隔离运行依赖。安装器的可见交互、发布者签名、最低系统和真实系统双击打开继续独立验收。

该工作流默认 `scope=fixtures` 保持上述夹具范围；`scope=product` 只在 Windows 构建实际产品，生成并核验真实载荷、安装器与逐文件许可证据，再通过实际安装→安装目录运行→升级→再次运行→卸载。运行使用外置的隐藏原生应用服务探针，检查安装目录的 Qt DLL 加载位置、编辑／撤销／保存和模式切换；另验证实际 Qt DLL 被修改时拒绝维护、保留自建数据和默认应用不变。报告明确记录当前用户候选注册确实发生于一次性 CI 账户；不在用户账户执行此项。工件只上传证据，不公开上传产品二进制。此范围仍不包含可见向导、Explorer／默认应用交互、最低系统或正式发行验收。
