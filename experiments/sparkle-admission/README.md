# Sparkle 原生准入探针

`probe.m` 是独立 Foundation 诊断程序，由 `scripts/check_sparkle.py` 在本项目自有临时 bundle 中编译和运行；不是产品更新器。它验证已准备的 Sparkle 主库、共享会话配置及 macOS 13 兼容候选的本机代理／TLS 路径，不执行安装或真实公开更新检查。

固定输入、构建命令、许可、macOS 13 研究要求及证据边界见[macOS 更新网络准入](../../docs/engineering/macos-update-transport.md)。该入口不启动 Qt，不读取或修改个人文档及系统代理；自建证书私钥和临时探针随夹具结束删除，报告保留在指定的 `build/` 目录。
