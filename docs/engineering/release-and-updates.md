# 发行渠道、许可核验与更新

发行决策以 [release-policy.json](../../tools/release-policy.json) 为机器可读依据。当前已实现开发安装维护、依赖材料核对、macOS ad-hoc 完整性检查及开发归档；[Rust 更新核心](../../crates/update/src/lib.rs)通过桌面 C ABI 接入关于页的主动检查与受限网络请求。清单和 Sparkle 已配置各自的生产公钥。工程已接入认证包下载、文档任务交接、Sparkle 原生提供方及 Windows／Linux 独立助手。公开受信候选、正式包的安装／恢复和商店交付仍待验收。

## 渠道与签名

| 渠道 | 既定方式 | 更新归属 |
| --- | --- | --- |
| Windows 普通安装器／便携包 | 不自行办理发布者数字证书或 Authenticode 签名，明确未签名状态 | 经独立信任校验的安装版后续接入更新；便携或身份不明副本提供手动入口 |
| Windows Microsoft Store | 后续构建 MSIX，使用 Partner Center 的真实包身份，Store 处理分发包签名 | Store 管理更新，包身份下隐藏并拒绝应用内 GitHub 自更新 |
| macOS | 不办理 Developer ID 签名或 Apple 公证；保留应用、framework、XPC 与 helper 所需的 ad-hoc 完整性封印 | 后续接入 Sparkle 原生更新及独立 Ed25519 真实性验证 |
| Linux | 保留原生依赖审计、实际许可与安装归属 | 后续复用签名清单及独立原生包助手；便携副本提供手动入口 |

MSIX 的商店处理方式见[微软说明](https://learn.microsoft.com/en-us/windows/msix/package/sign-msix-package-guide)。不将 EXE／MSI 上架的证书条件套用于 MSIX，也不承诺未签名 MSIX 能直接侧载。Store 的 Identity、Publisher 与产品标识必须采用真实分配值，未取得时保留待绑定项，不编造。

Windows 发布者证书、macOS Developer ID 和 Apple 公证不列为办理待办或发行阻碍。更新数据的签名验证独立保留：它确认候选来自本项目更新信任根，与系统发布者身份签名分别处理。依赖库原有签名按来源和完整性要求保留，不因本项目不办理证书而随意移除。

## 生产更新签名

发行负责人在受保护的 `release-signing` GitHub Actions 环境配置两套独立的 Ed25519 私钥。公开 Key ID 使用环境变量。两份公钥随源码编入客户端与签名工具。

| 环境配置 | 类型 | 用途 |
| --- | --- | --- |
| `XDNOTE_UPDATE_KEY_ID` | Variable | `xdnote-manifest-v1`，应在[清单公钥集](../../tools/update-trust.json)中匹配一把公钥 |
| `XDNOTE_UPDATE_SIGNING_KEY_HEX` | Secret | 清单 Ed25519 私钥，64 个小写十六进制字符 |
| `XDNOTE_SPARKLE_SIGNING_KEY_HEX` | Secret | 归档及 appcast Ed25519 私钥，64 个小写十六进制字符，对应 [Sparkle 公钥](../../tools/sparkle-update-key.json) |

配置或更换 Secret 后，在 `main` 手动启动 [Check production update keys](../../.github/workflows/release-key-check.yml)。`release-signing` 审批通过后，两个步骤分别派生公钥并与编入工具的配置比较。步骤状态给出各自的结果，日志只提供固定错误代码，工作流不生成签名或发行工件。

[Release update signing](../../.github/workflows/release-signing.yml) 接收已核验的 `release-verified` Actions 工件。其根目录包含 `identity.json`、`verification.json` 和 `assets/`，输入契约与[候选清单入口](../../scripts/release_candidate.py)一致。签名和独立验签使用同一个工件 ID，且候选构建身份须与 `main` 上冻结的签名提交一致。候选流水线可在同一次手动运行中调用此工作流，也可指定同提交的成功候选运行。现有开发部署工作流只交付证据，完整发行候选的产出仍按原生验收及分发许可待办推进。

工作流先确认签名环境已设置必需审批人，再在无密钥的步骤编译锁定工具。审批后的签名步骤先核对私钥与已配置公钥，再签 Sparkle 归档并生成 `macos-update.json`。随后从同一个候选构建并签署 `appcast.xml`，对 `release.json` 的原始字节生成 `release.json.sig`。`SHA256SUMS` 覆盖六个资产和全部签名附件。独立作业下载同一候选和已签名附件，使用编入的公开信任根再次核对包、签名及校验和。

本地可用已保存的 TXT 调用同一工具。准备项目锁定的 Rust 工具链和依赖后，使用以下入口；两种路径采用相同的候选校验和签名协议。

```bash
python scripts/sign_release_offline.py \
  --keys /path/to/xdnote-update-keys-v1.txt \
  --identity build/release/identity.json \
  --assets build/release/assets \
  --verification build/release/verification.json \
  --output build/release/signed-metadata
```

本地独立验签使用 `python scripts/release_updates.py verify`，传入同一组 `--identity`、`--assets`、`--verification` 和 `--output`。签名结果仍需按适用的原生验收、许可及发行授权流程处理。

## 用户主动更新契约

采用既有的候选选择、清单认证、网络边界、原生交接及恢复方法，按本项目身份移植；Qt Widgets 窗口承接状态与操作，不另起更新协议。检查由用户从关于页主动触发，不在启动时轮询、不静默安装；跨窗口共享唯一控制器，关闭关于页不丢失操作状态。

- 固定公开仓库为 `xiaodaozhi/xdnote`，按语义版本、渠道和单调 build 选择同一候选，核对真实包型、CPU、最低系统、资产长度及安装身份。版本与 build 均须递增，不按发布时间推断。稳定版只收稳定版；alpha 接受 alpha／beta／rc／stable，beta 不接受 alpha，rc 只收 rc／stable，dev 可以检查公开版本。当前没有独立 CLI，不引入不存在的包型。
- 只访问固定 GitHub／下载 CDN 的 HTTPS 白名单，不读取 GitHub 凭据，不上传文档、路径、最近记录或设置。每页最多 100 项、最多 10 页、响应最多 8 MiB、清单 256 KiB、签名 4 KiB；重定向最多 5 次，元数据无数据／总时限为 15／30 秒，下载为 30 秒／1 小时。ETag 缓存最多 24 项，304 仅复用成功响应，重复点击不并行请求。
- 沿用系统、直连及无凭据 HTTP／SOCKS5 自定义代理和 HTTPS 镜像策略。操作开始时固定配置，失败不回退直连；TLS 验证保持启用。403 仅在有效限流证据存在时归为限流，429 为限流；其他离线、超时、TLS、损坏或缺包均为失败，不显示“已是最新”，不自动重试。
- 清单按原始 UTF-8 字节做 Ed25519 验证，信任根编入本项目客户端及独立助手；不能从下载内容增添信任根，不复用其他产品的生产公钥、私钥或身份。密钥交付与生产验收单独进行，不在源码或普通 CI 放私钥。缺少生产信任根时只提供手动入口，不自动覆盖。
- Windows／Linux 包流式写入私有缓存，长度及 SHA-256 都匹配后才交给助手，包上限为 2 GiB。开发、dirty、源码、便携或身份不明构建只检查／下载。MSIX 包身份单独处理；身份查询异常按包身份处理，避免误走普通安装器。

## 更新核心边界与验证

`xdnote-update` 是不依赖 Qt 的 Rust 库，保持既定 `schema_version: 1` 清单及独立签名信封，尚无网络、文件写入、解包或安装入口。清单字段为 `product`、`version`、`channel`、`build`、`tag`、40 位小写十六进制 `commit`、固定 `repository`、完整 `assets` 和可选 `macos_appcast_url`。`component` 只允许省略或空串；不接受 CLI。`assets` 每项包含 `filename`、`platform`、`architecture`、`package_type`、`minimum_system`、`bytes`、小写 `sha256` 和固定版本的 `url`；未知字段、重复结构字段、缺失或重复目标均拒绝。

三套平台配置的目标包型如下。它们仅定义候选识别和后续打包契约，不表示已有对应发行包，也不改变现有开发归档入口或其命名。文件名统一为 `xdnote-{version}-{后缀}`，下载 URL 严格绑定本仓库的 `releases/download/v{version}/{filename}`。

| 配置／包型 | 文件后缀 | `minimum_system` 精确字段 |
| --- | --- | --- |
| Windows x86_64：zip／exe | `windows-x86_64.zip`／`windows-x86_64-setup.exe` | `windows: 10.0.17763` |
| macOS arm64：app.zip／dmg | `macos-arm64.app.zip`／`macos-arm64.dmg` | `macos: 14.0` |
| Linux x86_64：tar.gz／deb | `linux-x86_64.tar.gz`／`ubuntu22.04-x86_64.deb` | `distribution: ubuntu`、`version: 22.04`、`glibc: 2.35`、`display_server: Wayland/X11` |

清单须同时包含上述六项目标，包长在 1 字节至 2 GiB 内。系统字段是[既定平台目标](platform-support.md)的匹配信息，不是宿主探测或最低系统通过记录。macOS feed 若提供，必须绑定同一 tag 下的 `appcast.xml`；后续 Sparkle 接入仍须选择同一个版本与架构。MSIX 独立交由 Store，不在自更新目标中。

版本的三个数字允许 0–65535，预发行序号和发行 build 为 1–65535。公开候选只接受 stable 或 `alpha.N`／`beta.N`／`rc.N`，不接受 build metadata 或 dev。当前开发安装器使用的 `0.1.0-dev.<12位提交号>`可作为本地比较基线，提交号不参与大小排序；开发渠道可显式传 `current_build=0` 表示尚无发行计数，正式渠道不允许零。接入产品时须读取真实构建身份，不创建虚构发行版本或将开发包改成自动安装候选。

`inspect_release` 的 `valid` 仅表示结构和目标匹配。`verify_manifest` 使用调用方提供的本地可信 keyring 对原始字节认证；不得将下载得到的 keyring 传入。产品入口 `authenticate_release` 只使用编入库的 [update-trust.json](../../tools/update-trust.json)。它仅为签名、版本及 build 均通过的新候选返回不可改写的 `AuthenticatedRelease`。该对象的 `verify_package` 逐块校验实际长度和 SHA-256，最多读到声明长度外一字节即拒绝；安装交接时仍须复验实际文件与运行身份。空生产 keyring 始终拒绝，私有签名夹具只编入单元测试，不进入生产信任材料。

无需 Qt SDK 即可验证核心；Rust 版本遵循 [developer-build.md](developer-build.md)，Cargo 缓存和输出放在本项目 `build/`。完成 `cargo fetch --locked` 的显式准备后，运行 `cargo fmt --all -- --check`、`cargo clippy --locked --offline --workspace --all-targets -- -D warnings`、`cargo test --locked --offline --workspace`。CI 使用手动的 [Update core](../../.github/workflows/update-core.yml)，只检查 Rust 工作区与记录的依赖材料，不运行桌面矩阵或发布产品。覆盖候选／渠道、单调 build、三平台包型／最低目标、损坏或不可信签名、空信任根、长度／摘要与流读取失败；网络 TLS、活动文档、原生安装／恢复和真实平台继续单独验收。

## 主动检查的开发实现

构建通过 `scripts/build_version.py` 在配置和构建时生成实际 Git 提交、工作区 dirty、CMake 配置与 `0.1.0-dev.<12位提交号>`，写入所选构建目录的 `generated/build-version.json` 并编入 `BuildInfo`。默认仍为开发渠道、build 0；源码归档或无法确认当前仓库时记录 unknown。显式 `desktop.py build --release-identity <文件>` 才启用发行身份输入，文件包含 `schemaVersion: 1`、`product: xdnote`、`version`、`build`、完整 `revision`、`previousVersion`、`previousBuild`；必须是当前 clean Release 提交，公开语义版本和 1–65535 的 build 均严格递增。上次发行记录由受控发行过程提供，不从 Git 时间或提交数量推断，不因启用该入口创建发行记录。

`FileApplication` 持有唯一 `UpdateController`；启动、打开文档或关于页不会发出请求。用户点击后按语义版本排序公开候选，核对同一 tag 的 `release.json`、目标包的名称／URL／长度及可选 `release.json.sig`，不因最高候选损坏或缺包而回退为“已是最新”。检查中重复点击不创建并行请求；取消和销毁递增代次，旧回执不能覆盖后续结果。设置窗口重开连接同一个控制器，文档原文、撤销和保存任务不参与检查流程。

桌面通过 `xdnote-services` 将 Markdown 与更新核心合为一个 Rust 运行库归档，源模块及测试继续独立。薄 C ABI 使用有界 JSON 请求／自有输出分配，清单与签名以十六进制传递原始字节，避免 Qt 字符串解码改变签名字节或 BOM。ABI 不接收运行时信任根；`authenticate` ABI 将签名、新版本／build 和平台包型一起认证；当构建未配置生产 keyring，或签名缺失、密钥未知时，只提供手动发布页，下载与安装按钮保持不可用。

`QNetworkAccessManager` 只用于这一个控制器，关闭 Cookie 读写及身份缓存复用，不读取 GitHub 凭据。每个请求保留 TLS 校验、15 秒无数据／30 秒总时限、5 次已核对重定向；按 64 KiB 读块执行各类响应上限，结束时仍读取末尾数据并检查。ETag 最多保留 24 条成功 HTTP 响应，304 必须有对应的成功缓存；TLS、超时、限流、损坏和缺包分别保留失败，均不显示为当前无更新。Qt 行为依据见[请求](https://doc.qt.io/qt-6.8/qnetworkrequest.html)与[回执](https://doc.qt.io/qt-6.8/qnetworkreply.html)，实际依赖仍固定 6.8.2。

网络层支持系统／直连／无凭据 HTTP 或 SOCKS5 自定义代理及 HTTPS 镜像校验，配置只在操作开始时快照生效；失败不会自动重试或改为直连。应用级偏好已保留 `updates` 分组并绑定单一控制器，缺省仍用系统代理；已存非法配置阻止新检查，修正前保留原值。「关于 → 更新网络」提供整组草稿／保存入口，未保存值不进入请求；当前操作继续使用原快照。真实代理／PAC、最低系统与生产候选更新验收仍独立保留。Windows 通过系统包身份查询隐藏并拒绝 GitHub 更新操作，仅确定 `APPMODEL_ERROR_NO_PACKAGE` 才走普通副本，其他结果保守按 Store 处理；没有编造 Store 地址。查询依据见[微软说明](https://learn.microsoft.com/en-us/windows/win32/api/appmodel/nf-appmodel-getcurrentpackagefullname)，实际签名包仍需另验。

使用同一构建目录和 Qt SDK 执行 `python scripts/desktop.py test --suite updates`：固定离屏运行网络回执夹具、Rust ABI 及设置回归，证据进入 `evidence/updates/`。夹具覆盖成功／304、取消／重开、重复点击、分页／响应边界、TLS／限流／重定向、候选和资产绑定；合成回执不等于真实 HTTPS 服务器、代理路由或 Store 安装验收。桌面 CI 的手动 `scope=updates` 只补验三套默认配置的相同范围，保留未执行的人工视觉、真实网络和原生更新边界。

显式真实网络诊断使用同一构建的 `python scripts/desktop.py test --suite updates-live`，只请求固定公开仓库，输出实际状态／错误，不读取文档或偏好、不下载包、不安装，也不进入普通 CI。该入口的结果须单独记录。`scripts/check_update_network.py --build-dir <构建> --qt-prefix <Qt> --output <build 下的新目录>` 另以固定公开发布页和检查 API 记录系统、直连及本项目临时 HTTP CONNECT／SOCKS5 隧道；只记录状态、长度和固定目标去向，不记录响应正文、凭据或个人数据。发布页 HTTPS 200 与候选 API 检查成功分别判定，API 限流不计为更新检查通过。离屏夹具不能替代真实网络，当前宿主的网络结果也不代表最低系统或全部实际代理配置。

## macOS 原生实现

采用 Sparkle 2.10.0 和既有受限代理适配，通过薄 Objective-C++ 桥接 Qt Widgets 统一状态。源码、补丁、资源、helper、签名工具及摘要锁定在本项目，覆盖已定 macOS 14+、Apple Silicon arm64；框架主库与代理适配分别按架构构建和准入，新增依赖后核对实际载荷与许可。实现依据见 [Sparkle 官方文档](https://sparkle-project.org/documentation/)。

原生自动安装更新的准入从 macOS 14 开始，系统代理、直连及无凭据 HTTP CONNECT／SOCKS5 均使用已确认的受限适配；低于该范围不承诺原生自动安装，不继续以旧 CFNetwork 自定义代理方案扩展支持。固定依赖与独立探针见[原生更新网络准入](macos-update-transport.md)。macOS 桌面现在链接本项目显式准备、核对的 Sparkle；配置阶段只验证已准备的 SDK，不隐式下载。部署补齐实际 framework、原始许可／来源和 arm64 指令节证据，未使用的 downloader／installer-launcher XPC 不启用或交付；Autoupdate、Updater 和 framework 按各自 entitlements／runtime 参数由内至外重新封印。链接或生命周期夹具不等于已完成真实安装更新。

feed 只暴露已选定的同一候选。部署显式启用 `SURequireSignedFeed`、`SUVerifyUpdateBeforeExtraction` 并将 `SUSignedFeedFailureExpirationInterval` 设为 0；不接受 feed 验签被跳过、失败或超时降级。由 Sparkle 原生验证 feed／归档并处理实际替换，不声称 Sparkle 执行了额外 SHA-256。只读 DMG、App Translocation、未登记 `.app` 或缺少公钥使用手动入口。DMG 与 app.zip 应来自同一个已部署、完整性签名及核验的 `.app`，解包后另做重定位验证。

系统／直连／无凭据 HTTP CONNECT／SOCKS5 策略在实例启动前固定，feed 与归档共享同一会话；直连显式关闭系统代理、PAC 和自动发现，不以空字典冒充关闭。自定义代理及系统 PAC 选中的代理失败时均报告下载错误。系统 PAC 按每个请求及重定向目标重新解析。Framework、XPC 和 helper 保留各自参数及 entitlements，不套用通用递归重签规则。

## 文档保护、交接与恢复

检查和下载期间文档继续工作。确认安装后复用未保存保护，让用户保存、放弃或取消；不得隐式丢弃修改，不为等待安装取消已有文件操作。进入交接后拒绝新会话和新任务，等待保存、资源、解析及所有连接清理、join 完成，再提交安装。

用户选择“不保存”的修订在提交前从恢复草稿快照中移除，并等待该记录持久化；内存原文和撤销仍保持冻结。若交接取消或提交失败，恢复原有草稿和同一缓冲区；成功更新退出后，不再将已明确放弃的内容当作可恢复修改。

冻结前无交互地复核所有当前窗口及待完成打开；后续询问期间发生的新编辑使先前决定失效并重新确认。取消使该轮对话框的迟到回答失效，不能将旧轮“不保存”带入下一次更新。

Windows 独立助手复验清单、长度和摘要，持有安装门并等待原进程退出后进入原有安装器；Linux 原生系统包路径使用固定、受限的权限助手，不接收任意命令或包名。用户级开发维护工具继续保持自身目录与权限边界，不因此自动改成系统级安装。

macOS 全部 join 后才释放 Sparkle 安装回调，只有实际 committed 后才放开安装退出；取消不留下“下次退出安装”计划。取消或准备失败须等待原生周期结束，并精确观察本应用 helper 已退出后才允许普通退出或重试，不杀 helper、不读取无关进程信息。

尝试记录与更新回执独立于用户偏好，绑定事务、目标版本／build、运行路径和安装身份。重启按实际运行身份核对成功；失败、缺回执或身份冲突进入恢复，不自动重试。设置、未知键、默认关联、用户数据和无关文件都要保留；下载或提交 accepted 不代表安装成功。

Windows／Linux 从启动独立助手起保留该事务缓存，取消或退出不能先删除控制文件，使助手丢失取消标记。重试使用全新下载目录，不复用旧事务的 helper／回执。当前保留的事务材料可用于恢复排查，尚不自动回收；未交给助手的下载和半包仍由界面取消清理。


## 下载、原生交接与工程包型

认证候选可显式下载；检查与下载复用开始时固定的网络会话和配置，不会在失败后回退直连。包按 64 KiB 写入独立的 `job-*` 私有目录和 `QSaveFile`，精确长度、2 GiB 上限及 SHA-256 全部匹配后才提交缓存文件。下载保留 TLS 1.2+／证书验证、Cookie／身份复用关闭、五次已核对重定向、30 秒无数据和一小时总时限；HTTP 错误、尾部额外数据、截断、摘要不符、磁盘失败和取消均不能进入已下载状态。开发或身份不明副本可下载已认证包，不能自动安装。

安装提供方只在本地身份匹配时开放：Windows 必须匹配自有 InstallId、登记路径和安装清单，Linux 必须位于 root 拥有的版本目录并经 dpkg 包归属复验，macOS 必须匹配已登记 bundle、公钥、非只读卷及受支持的 Applications 目录。部署从 `tools/sparkle-update-key.json` 读取 32 字节 Base64 公钥并写入 bundle，开发副本不标为可安装。MSIX 身份下拒绝独立自更新。

文档准备逐份复用保存／不保存／取消确认，不提前关闭窗口或清空撤销；不保存选择绑定当时 revision。取消后仍保留同一会话和撤销，已经明确完成的保存保持有效。全部确认后冻结修改、拒绝新打开和后台任务，已有保存／资源操作正常完成，再非阻塞等待全局工作池 join 并刷出恢复记录。此后才写交接记录并释放 native commit；交接失败会恢复原只读及启用状态。Sparkle 取消用 Skip，不能留下退出时安装计划；普通退出和重试继续等待原生周期及本应用准确 helper 退出。

独立 `xdnote-update-helper` 不依赖 Qt。它重新认证原始清单／签名、核对当前安装身份和单调版本，持有包文件并复核长度／SHA-256；只接受固定请求格式、事务和目录内文件。Windows 运行已认证的固定安装器并保留更新事务互斥；Linux 经固定 polkit 权限入口复制到 root 私有缓存后复验，仅调用受限 apt 包安装。Linux 重启以原用户身份执行并保留原进程的桌面会话变量，包括 Wayland 套接字名和会话类型。包助手、安装器与应用共用运行／维护门，不终止持有未保存文档的进程。尝试记录独立于偏好，重启还须核对完整提交、版本／build、运行路径及助手回执；accepted 不显示为安装成功。记录缺失或冲突进入恢复，不自动重试。

工程打包入口 `scripts/build_linux_installer.py` 生成有实际归属、固定权限策略和保守维护门的 Ubuntu 22.04 amd64 开发包；`scripts/msix_package.py` 只在调用方显式提供 Identity／Publisher／PublisherDisplayName 及正确素材时调用 Windows SDK MakeAppx，生成未签名 MSIX，不签名、不安装或提交 Store。`scripts/check_msix.py` 仅在一次性 Windows CI 使用明确标记的工程身份检验 XML／资产与 MakeAppx，不能将它当作真实 Partner Center 身份。Store 偏好按实际 package family 分隔，包身份失败也与普通安装版分隔；真实并存／激活与默认应用保留仍需安装验收。

正式 MSIX 的三个包身份字段按 Partner Center 的产品标识页原样记录到 JSON，键为 `name`、`publisher` 和 `publisherDisplayName`。GitHub Actions 仓库变量 `XDNOTE_MSIX_IDENTITY_JSON` 保存该配置，本地持久副本位于主工作区的 `build/release/partner-center/msix-identity.json`，用 `scripts/msix_package.py --identity` 显式传入。受控的 Actions 打包任务通过环境变量接收配置并写入其 `build/` 下的输入文件，避免将值展开到脚本或输出到日志。工程 MSIX 检查继续使用夹具身份。字段来源见[微软的产品标识说明](https://learn.microsoft.com/en-us/windows/apps/publish/view-app-identity-details)。

`scripts/release_candidate.py` 只为六个实际资产生成既定 schema-1 未签名清单，要求同一 clean 发行身份及逐包摘要／长度验证记录，不创建 tag、Release 或签名。Windows 数字版本、macOS CFBundleVersion 和 Debian 包版本分别绑定生成身份；当前默认仍没有正式发行 build。所有包型继续需要各自最低系统、真实升级／恢复和最终许可核验。

## 许可核验与准入

沿用实际文件归属、固定来源／摘要、原始版权和许可证、SPDX、代码／数据节比较、源码获取及隔离替换运行的方法。只核对实际使用的 Qt 模块、Rust 静态组件、Scintilla、运行库和新增 Sparkle；不能把 SDK 全部条目、其他产品字体或包级版权清单当成实际编入范围。

已识别且适用的义务或必需材料未解决时阻断对应候选；上游 `NOASSERTION` 或机器报告不完整本身不能直接推导为许可冲突。材料检查、运行验证与最终许可判断分别记录，不因复用流程而自动清除未完成项。Qt 源码不进入 Release、安装包或应用归档，适用源码获取与替换安排独立落实，见[分发核验](distribution-audit.md)与[替换说明](qt-library-replacement.md)。

准入沿用候选／签名／TLS／损坏包夹具及真实更新、取消、失败恢复、重启身份／设置保留的定向验证；私有测试密钥不污染生产材料。macOS 的原生替换、取消 helper 退出、代理去向和无 SDK HTTPS 分别留证。普通提交只运行日常检查，按影响补验，不重复完整桌面矩阵，不以 CI 编译替代实际更新或商店安装。

macOS 的[私有原生演练](../../experiments/native-update-fixture/README.md)以临时编译信任根构建两个本地测试版本，自动核对恢复、损坏包、取消与准确 helper 退出、真实替换及重启后完整身份／文档／未知设置键保留。签名种子在执行前删除，生产信任材料摘要必须不变；本地回环 native feed、合成的其他平台资产和安装根例外只存在于隔离 worktree，不把它们当作生产或最低系统通过记录。

## 只读候选预检

`scripts/release_preflight.py` 复用六种真实资产与封存摘要的候选契约，核对 clean Release 版本／build／提交和同一组文件。它不签名、上传、打标签或发布；没有原生包验收、完整许可／源码履行、最低系统／真实桌面和具体发行授权时，报告持续保留 `releaseReady=false`、`licenseReviewComplete=false`。元数据通过与身份验签通过分别记录，不把验证 JSON 的 `verified` 字段当作最终发行授权。

可选验签使用 `crates/update/examples/verify_release.rs`：以固定 Rust 工具链在本项目 build 的 target-dir 编译后，传入原始清单与签名信封两个文件路径。该命令只调用既有 Rust 核心和编译时生产公钥，不接受运行时 keyring、私钥或网络输入；读取先受元数据上限约束，输出清单与公钥输入摘要及稳定错误码，失败退出码为 2。生产根为空时拒绝认证。

预检的 `--manifest`、`--signature`、`--verifier` 必须一起提供；工具核对它们与六资产候选、当前公钥文件及项目自有验证程序的一致性，拒绝陈旧或矛盾回执。`--require-authenticated` 可将未通过身份认证作为非零退出，但仍不代表其他发行门槛已完成。清单签名与 Store 包身份按各自的发行输入提供。
