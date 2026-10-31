# 参与 xdnote 产研

先阅读[文档导航](docs/README.md)、[产研流程](docs/engineering/workflow.md)和受影响的产品／视觉规格。仓库仍处于设计与技术验证阶段；实验代码不是产品实现。

1. 在 [Issues](https://github.com/xiaodaozhi/xdnote/issues) 查重，需求、Bug、设计或研究按模板建立或关联任务，写明范围和验收。
2. 从最新 `main` 创建 `feat/<issue>-<slug>`、`fix/<issue>-<slug>`、`design/<issue>-<slug>`、`docs/<issue>-<slug>` 或 `chore/<issue>-<slug>` 短期分支。
3. 执行受影响的本地检查，更新相关规格；分别记录材料检查、实验测试、产品功能与真实平台／人工验证。
4. 创建关联 Issue 的 PR，写清变更、验证和未完成项。全部验收完成才使用 `Closes #编号`；部分交付使用 `Refs #编号` 并保持 Issue 开放。
5. 等待当前 PR head 的适用 CI、必要审查及 Issue 验收通过后合并到 `main`。核对合并结果，安全删除已合并的临时远端／本地分支，并在工作区干净时快进同步本地 `main`。

已授权的实施任务包含提交、推送、PR 和满足条件后的合并；用户另行限定仅设计、暂不合并或存在未满足的验收时，停在指定边界。不得删除他人的工作、未合并分支或无关数据。

自有软件贡献按 Apache-2.0，自有说明文档和原创设计贡献按 CC BY 4.0；提交第三方内容时保留其来源和适用许可。内容范围、署名及品牌权利边界见[许可说明](docs/engineering/licensing.md)。工程选型和验证遵循[平台目标](docs/engineering/platform-support.md)，当前图组与共享规格是已授权的开发基线。

静态材料检查需要 Node.js 及锁定的开发依赖：

```sh
npm ci
npm run check:materials
python scripts/check_repository.py
```

Qt 试验的工具和入口见[试验说明](experiments/qt-document-spike/README.md)。这些命令不构建正式应用，CI 不代替原生桌面、输入法、辅助功能或人工视觉验收。
