# xdzip 实践参考

查阅对象为 2026-10-29 可访问的 xdzip 本地工作树，依据是实际文档、源码结构和工作流配置，不把历史聊天结论当作当前实现证明。本次未构建、测试或更改 xdzip；没有核定其当前提交及远端同步状态。所用文件的内容摘要保存在[参考来源清单](xdzip-reference-sources.json)，便于日后区分参考版本。

## 可以直接采用的方法

| 观察到的做法 | xdzip 实际依据 | 在 xdnote 中采用的方式 |
| --- | --- | --- |
| 持久规范与原稿分离 | `README.md`、`docs/README.md`、`docs/engineering/repository-hygiene.md` | `docs` 保存规则，`design/hifi` 保存当前基线，运行证据另存 |
| 参数、窗口登记与设计流程先行 | `docs/visual/design-standard.md`、`design-workflow.md`、`layout-scale.json` | 创建自己的 tokens 和窗口清单，尺寸为逻辑单位，不照抄 xdzip 的数值 |
| 分别测量主界面、菜单和独立入口 | 视觉规范明确区分密度、DPR、辅助字号与独立窗口 | 三类窗口与菜单共同验收，不用缩小截图或二次乘缩放修复布局 |
| 用统一意图连接鼠标、键盘和辅助功能 | `docs/ux/settings-localization.md` 的单选、复选与下拉契约 | 主题、六色单选和开关更新同一设置模型，不能只改勾选图形 |
| 所有文件入口经过会话协调 | `docs/engineering/application-sessions.md`、`apps/desktop/ApplicationSessions.h` | 统一 OpenIntent 和文档去重，不让双击／拖入／工作区各开一套缓冲区 |
| GUI 与领域分层 | `Cargo.toml`、`CMakeLists.txt`、`docs/engineering/repository-layout.md` | 将职责作为架构候选；最终编辑模型与语言边界先验证再确定 |
| 本地与 CI 共用分阶段入口 | `scripts/desktop.py`、`docs/engineering/developer-build.md` | 技术选定后再实现统一入口；不现在伪造一份不可运行的构建说明 |
| 验证按改动类型分层 | `docs/engineering/ci-policy.md`、`.github/workflows/qt-components.yml` | 文档检查、核心测试、原生行为与视觉验收分开，不能以 CI 状态推定人工验收 |
| 开发就绪与发行就绪分别评审 | `docs/planning/readiness.md`、`docs/engineering/release-protocol.md` | 阶段完成不自动变成可公开发行，不把打包成功写成许可和安装通过 |

表中有些依据是仓库契约，有些是代码或流水线配置。它们证明项目采用的结构和方法，不证明本次已经观察到运行结果。

## 需要适配，不能照搬的部分

### 文档状态比归档浏览更敏感

xdzip 的会话主要围绕归档浏览与后台任务。xdnote 要持续保留尚未写入磁盘的文本、输入法组合、撤销栈与保存版本。可以借鉴稳定会话身份和统一打开路由，不能复制“关闭页面后任务继续”的模型来替代文档关闭保护，也不默认引入托盘常驻。

### 设置提交语义由本产品决定

xdzip 的相关契约采用设置草稿、保存和取消；本轮 xdnote 设计是即时生效和「完成」。借鉴共享控件及设置控制器，保留 xdnote 自己的即时提交与持久化失败语义，不能把两套交互同时放在一个窗口。

### 平台包必须按实际依赖核验

xdzip 的安装文档描述了应用目录内 VC++ 运行库、Qt Quick 样式裁剪、图形兼容库保留与独立部署核验。可借鉴“从实际依赖裁剪、绑定哈希、隔离 SDK 运行”的方法；具体 DLL、SDK、Qt 版本、字体、原始许可与再分发材料必须为 xdnote 自行确认。

xdzip 的最低平台、签名状态、Mac 普通 CI 与专用 Release 的不同策略都是该项目的现行约定，不自动成为 xdnote 政策。本文不复用其许可适用性结论，不把它的安装器或图标当作可直接发布资产。

### 安装注册与用户默认选择分开

`docs/engineering/installers-associations.md` 区分打开方式注册、默认应用、运行时检查及安装／升级／卸载验收。xdnote 同样区分这些步骤：注册 `.md` 并不授权抢占默认应用；卸载不能删除用户文档。具体平台实现仍待自己的验证。

## 当前采用结果

当前只落实可复用的材料结构与契约：文档入口、需求映射、六色参数、窗口清单、原稿摘要、持久／生成内容边界和分层验收方法。没有搬入 xdzip 的源码、品牌图标、字体、锁文件、构建产物、公共发行声明或历史验收记录。

后续选型应先完成[架构验证](architecture.md)，按[开发路线](../planning/roadmap.md)进入实现，不能把本页作为框架已批准或平台验证已通过的记录。
