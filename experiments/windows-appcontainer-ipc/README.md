# Windows AppContainer 单实例研究夹具

关联 #268。此试验使用 Windows、Qt 6.8.2 和 `apps/desktop/src/SingleInstance.cpp`，研究受限 AppContainer 中用户专属命名管道的监听失败；它不是产品修复，不改变产品 IPC 或访问控制。

在仓库根目录运行，先准备项目锁定的 Qt 6.8.2、MSVC v143、CMake 和 Ninja。`--vcvars` 指向本机 VS 2022 的 `VC/Auxiliary/Build/vcvars64.bat`；仅构建可在正常权限下执行，验证须在目标 AppContainer 中执行：

```powershell
python experiments/windows-appcontainer-ipc/build.py --vcvars "<本机 vcvars64.bat 路径>"
python experiments/windows-appcontainer-ipc/run.py
python experiments/windows-appcontainer-ipc/recovery.py
```

构建和隔离样本放在 `build/cycle-268-appcontainer/`，Qt 路径只设置给测试子进程。脚本查询当前用户及容器 SID 构造自建管道的 ACL，但不打印或持久化 SID。它先对照默认安全描述符及用户专属 ACL，再以当前用户和当前容器的权限建立随机名称的首实例；真实 Qt 监听就绪后关闭引导句柄，测试三个串行、四个并行客户端、带中文和标点的绝对路径、超长请求拒绝与后续转发，以及正常停止。样本目录应在退出时为空并移除。

`recovery.py` 在随机隔离目录中终止自己启动的主进程，确认锁文件留存后重新建立引导管道，验证陈旧锁恢复及正常转发；脚本只清理该隔离目录中的锁文件，不操作其他进程或用户数据。

`cross_container.py` 从非 AppContainer 的当前用户进程运行，临时创建两个随机名称的 AppContainer 配置；脚本先核验每个子进程的实际容器身份，结束时关闭进程并删除所创建的配置。它检查自建全局管道的创建结果，再在 `LOCAL` 命名空间检查不同容器访问、分别创建同名管道，以及同一容器的第二实例和客户端。该脚本不输出或持久化用户和容器 SID。它检查的是原生管道与命名空间，并不启动 Qt 产品监听或测试跨用户访问。

```powershell
python experiments/windows-appcontainer-ipc/cross_container.py
```

结果仅覆盖运行时环境、指定 ACL 和夹具中的客户端，不证明现有 Qt 全局管道方案的跨用户／跨容器隔离、恶意预占、所有异常退出场景或正式产品安装。不同容器在 `LOCAL` 下找不到彼此的同名管道时，应将命名空间隔离与 ACL 拒绝分开解释，不能将其作为全局管道 DACL 的验证。Qt 的 Windows 监听器不能通过 `listen(qintptr)` 接管现成句柄；引导方案、竞争、完整安全描述符与可靠退出仍须单独验证。禁止以默认 ACL、Everyone 权限或减小队列而不验证接力来代替安全结论。
