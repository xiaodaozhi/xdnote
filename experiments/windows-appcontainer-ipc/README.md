# Windows AppContainer 单实例研究夹具

关联 #268。此试验使用 Windows、Qt 6.8.2 和 `apps/desktop/src/SingleInstance.cpp`，研究并验证受限 AppContainer 中的单实例管道。产品实现与原生对照分别测试，不将原生管道探针当作产品端到端验收。

在仓库根目录运行，先准备项目锁定的 Qt 6.8.2、MSVC v143、CMake 和 Ninja。`--vcvars` 指向本机 VS 2022 的 `VC/Auxiliary/Build/vcvars64.bat`；仅构建可在正常权限下执行，验证须在目标 AppContainer 中执行：

```powershell
python experiments/windows-appcontainer-ipc/build.py --vcvars "<本机 vcvars64.bat 路径>"
python experiments/windows-appcontainer-ipc/run.py
python experiments/windows-appcontainer-ipc/recovery.py
```

构建和隔离样本放在 `build/cycle-268-appcontainer/`，Qt 路径只设置给测试子进程。`run.py` 使用产品代码自己创建受限管道，测试三个串行、四个并行客户端、带中文和标点的绝对路径、超长请求拒绝与后续转发，以及正常停止。样本目录应在退出时为空并移除。

`recovery.py` 在随机隔离目录中终止自己启动的主进程，确认锁文件留存后重新启动产品单实例，验证陈旧锁恢复及正常转发；脚本只清理该隔离目录中的锁文件，不操作其他进程或用户数据。

`product_container.py` 从非 AppContainer 进程创建两个随机名称的临时容器，运行链接实际 `SingleInstance.cpp` 的探针。它验证已有同名管道时拒绝接管、另一个容器无法打开、顺序／并发转发，以及多个进程同时启动时仅产生一个主实例。测试专用编译标志允许暂停引导过程以验证早到客户端重试，也允许人为设置错误的预期进程 ID，核对打开意图未发送、后续正常客户端能够转发。它需要项目内 Windows 运行库 `build/toolchain/windows-runtime/runtime/`。测试将所需的 Qt 和运行库复制到专用目录，只为本次两个容器授予读取权限，退出时删除测试目录与容器配置。脚本不输出或持久化 SID。

```powershell
python experiments/windows-appcontainer-ipc/product_container.py
```

`cross_container.py` 从非 AppContainer 的当前用户进程运行，临时创建两个随机名称的 AppContainer 配置；脚本先核验每个子进程的实际容器身份，结束时关闭进程并删除所创建的配置。它检查自建全局管道的创建结果，再在 `LOCAL` 命名空间检查不同容器访问、分别创建同名管道，以及同一容器的第二实例和客户端。该脚本不输出或持久化用户和容器 SID。它检查的是原生管道与命名空间，并不启动 Qt 产品监听或测试跨用户访问。

```powershell
python experiments/windows-appcontainer-ipc/cross_container.py
```

`Desktop admission` 的 `scope=ipc-windows` 与 `ipc_cross_account=true` 启用跨账户定向检查。一次性托管 Windows 运行器创建标准测试账户，两个真实 Windows 用户分别启动相同 AppContainer 身份的进程。第一账户的真实产品监听器在检查前后均可由本账户打开。第二账户仅尝试打开同一管道，记录访问被拒或管道不可见这两种结果。脚本不发送打开意图，结束后清理临时账户和容器配置。此模式只运行跨账户检查。

结果仅覆盖运行时环境、指定 ACL 和夹具中的客户端；不同容器在 `LOCAL` 下找不到彼此的同名管道，应将命名空间隔离与 ACL 拒绝分开解释。预占拒绝不证明同一容器与用户身份下的恶意进程无法创建竞争实例；跨用户访问、所有竞争和异常退出场景、正式产品安装仍须分别验证。禁止以默认 ACL、Everyone 权限或减小队列而不验证接力来代替安全结论。
