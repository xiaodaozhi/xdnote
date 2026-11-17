# 私有 macOS 原生更新演练

本目录是可恢复的测试输入，不是产品构建入口或发行素材。`Fixture.mm.in` 仅编入工具在 `build/` 创建的独立 detached worktree；普通产品不链接这些入口。

准备本项目固定 Qt、Scintilla、Cargo 缓存及 Sparkle 后，从干净提交运行：

```text
python scripts/check_native_update.py --work-dir build/cycle-155-native-update/native-fixture --qt-prefix build/Qt/6.8.2/macos
```

工具核对同项目 Git 归属，在独立 worktree 中编译两个实际应用版本并建立本地测试提交。临时随机种子只保存在独立私有文件，用于为夹具编入公钥、签署清单、feed 和归档；不读 Keychain 或生产私钥。签完删除种子，执行阶段没有签名权限；生产信任文件的前后摘要必须相同。本地 200／201、alpha 版本、临时公钥和其他平台的合成资产记录均不能作为正式发行身份或生产信任材料。

元数据回执只映射固定原始 URL 到自建文件；Sparkle 的真实 feed／归档由限于两条路径的回环 HTTP 服务提供，测试 bundle 单独允许本地网络。公用候选匹配、编译信任根认证、原生 feed／归档验签、实际替换与重启继续执行；正常产品的 HTTPS、TLS 和代理策略不被改写。四种场景为缺失／冲突回执恢复、损坏归档、准备后的取消及 helper 精确退出、安装后完整版本／提交身份与文档／未知设置键保留。

桌面只通过私有 worktree 的 `scripts/desktop.py run` 启动，显式指定构建、Qt 及已封存的夹具 bundle；该额外参数不进入正常产品入口。样本、配置、尝试记录、签名材料和报告进入所选 `build/` 目录，证据中保留原提交、测试提交、模板摘要及实际结果。保留活动 worktree 和仍需证据，周期结束按[清理约定](../../docs/engineering/workflow.md)处理。

此演练可以在锁屏下自动完成，不读写个人文档、不改系统代理、不安装 `/Applications`，也不证明最低 macOS、真实代理设置／PAC、生产公钥、公开候选 API、商店或人工界面验收。模板会因源入口变化而明确拒绝应用补丁，不能放宽匹配或生产门槛来使测试通过。
