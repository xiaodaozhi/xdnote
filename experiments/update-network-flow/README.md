# 代理大流与缓存边界

这是仅使用 Qt Core／Network 的隔离诊断工程，不链接进产品。用于复验 [Issue #173](https://github.com/xiaodaozhi/xdnote/issues/173) 的高速代理通知、背压和 TLS 尾部读取。固定 SDK 的显式准备方法见[开发构建](../../docs/engineering/developer-build.md)。

```text
python scripts/check_qt_network_flow.py --qt-prefix build/Qt/6.8.2/macos --work-dir build/cycle-173-download-flow/flow
```

按实际平台替换 SDK 子目录；Windows 可传 `--vcvars`。输出目录必须不存在，所有服务仅绑定回环地址，代理只允许 `updates.fixture.invalid` 的自有端口。入口删除测试私钥，不修改证书存储、系统代理、生产信任根或已安装应用。

同一入口覆盖 HTTP forward／CONNECT 与 SOCKS5、真实 HTTPS、256 MiB 未限速持久连接、暂停后正常关闭、截断／损坏／超长拒绝，以及错误主机名和未受信 CA 拒绝。公开回复探针固定 64 KiB 接收缓冲、严格证书校验、长度和 SHA-256 门槛。私有 socket 探针只在此工程使用 Qt 6.8.2 私有头，分别采样 64 KiB 引擎／控制缓存和 4096 字节外层缓存，并覆盖事件与阻塞读取、持续连接和 EOF；不把私有 API 带入产品。

三平台手动工作流为 [qt-network-flow.yml](../../.github/workflows/qt-network-flow.yml)，只上传报告及日志。通过此诊断不代表原生安装、最低系统、真实公网或完整发行验收；实际产品另按[私有更新夹具](../native-update-fixture/README.md)验证验签、取消、保存和事务回执。单次结果记录在对应 Issue／PR。
