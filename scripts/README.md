# scripts —— 构建自动化

> ## 先回答一个常见疑问：官方到底支持什么？
>
> 官方文档 `zh-cn/quickstart/openvela_ubuntu_quick_start.md` 原文：
>
> > 本文仅适配 **Ubuntu 22.04**。不支持在 Windows Subsystem for Linux (WSL) 或
> > Docker 容器环境中进行编译。
>
> 更硬的证据是官方环境检测脚本 `detect-env.sh`（本目录 `official/` 下有完整副本），
> 它把判定**写死**在代码里：
>
> ```bash
> echo "【2/7】运行环境..."
> if grep -qi microsoft /proc/version 2>/dev/null; then
>     fail "WSL 环境 (不支持编译)"
> elif [ -f /.dockerenv ]; then
>     fail "Docker 环境 (不支持编译)"
> else
>     pass "原生 Linux"
> fi
> ```
>
> ### 判定规则汇总（来自官方脚本原文）
>
> | 检查项 | 通过 | 警告 | 失败 |
> |---|---|---|---|
> | 运行环境 | 原生 Linux | — | **WSL**（含 `microsoft` 标记）、**Docker**（有 `/.dockerenv`） |
> | 操作系统 | Ubuntu 22.04 | 其他版本 Ubuntu | 非 Ubuntu |
> | 内存 | ≥15360MB（15GB） | ≥7680MB（7.5GB） | <7680MB |
> | 磁盘 | ≥40GB | ≥20GB | <20GB |
> | 基础工具 | git / cmake / python3 / make / gcc | — | 缺任一项 |
> | 其他 | git-lfs（含 `lfs install`）、repo | lfs 未初始化 | 缺失 |
>
> ### 一个关键推论：虚拟机可以通过
>
> 检测脚本只查两件事：`/proc/version` 里有没有 `microsoft`、有没有 `/.dockerenv`。
>
> **VMware / VirtualBox 里跑的真 Ubuntu 两个标记都不匹配，会被判为「原生 Linux」并通过。**
>
> 所以润芯微的《在 VMware 中搭建 Ubuntu 22.04》教程可行，并不是"官方额外支持虚拟机"，
> 而是**检测器默认区分不出来，也不需要区分**——它关心的是宿主内核的文件系统语义，
> 而虚拟机里的 Ubuntu 内核与裸机没有区别。
>
> WSL2 其实也是真 Linux 内核，被排除的原因是**打标记**而非原理性缺陷。
> 但官方既然把它写进硬判定，说明确实踩过坑。**这是已知风险，不是误报。**
>
> ### 对你机器的具体判定
>
> | 项 | 你的机器 | 官方判定 |
> |---|---|---|
> | 操作系统 | Windows 11 Build 26200 | 需装 Ubuntu 22.04 |
> | 内存 | 15.3GB（约 15667MB） | ✅ **通过**（≥15360MB），但**贴近下限**，加 16GB swap 更稳 |
> | 磁盘 | C 盘剩 552GB | ✅ 通过（≥40GB） |
>
> ---
>
> ### 👉 已选定路径：云主机
>
> 见 **`云主机构建指南.md`** —— 含配置清单（照抄下单）、创建时的三个关键设置、费用预估。
>
> 本机环境已就绪：OpenSSH 客户端与 `~/.ssh/id_rsa` 密钥对都在。
> 云主机开好后，用 **`remote_build.ps1`** 一条命令远程完成全部构建：
>
> ```powershell
> powershell -ExecutionPolicy Bypass -File scripts/remote_build.ps1 `
>     -RemoteHost <公网IP> -User ubuntu
> ```

---

## 三个脚本的分工

| 脚本 | 运行在哪 | 作用 | 状态 |
|---|---|---|---|
| **`云主机构建指南.md`** | 你读 | 云主机怎么下单、怎么配 | ✅ 已选 |
| **`remote_build.ps1`** | Windows | 远程驱动云主机完成构建 | ✅ 已选 |
| `wsl_setup.ps1` | Windows（管理员） | 装 WSL2 + Ubuntu 22.04 | ⚠️ 官方检测会判 FAIL，已不推荐 |
| `build_openvela.sh` | Linux（云端/本地） | 装依赖 → sync → 编译 → 打包 | ✅ 被 remote_build 调用 |

`build_openvela.sh` 与 `remote_build.ps1` 是配套的：前者是"在 Linux 里干什么"，
后者是"怎么在远端驱动它并把产物取回来"。两者都能单独用。

这个目录里只有两个脚本，合起来把"搭建编译环境 + 编出固件"压缩成**你只需动手 5 分钟**。

---

## 为什么需要脚本

openvela 的编译条件很苛刻：**64 位 Ubuntu 22.04、磁盘 ≥80GB、内存建议 32GB**，
而且明确**不支持 WSL 和 Docker**。按官方路线你要：

1. 装 VMware
2. 下载 Ubuntu 22.04 ISO（约 4GB）
3. 图形界面安装系统、装 VMware Tools、配置共享目录
4. 在虚拟机里装十几个依赖包
5. 装 repo / git-lfs
6. `repo sync` 同步约 280 个仓库
7. 编译、打包

绝大多数时间耗在 1-4 步（环境搭建），而真正有价值的 5-7 步是纯命令行的。

**这两个脚本把 1-4 步换成一条命令，并把 5-7 步全自动化。**

---

## 你的操作（总共约 5 分钟 + 一次重启）

### 第 1 步：Windows 侧装 WSL（约 5 分钟 + 重启）

1. 右键「开始」按钮 → 选择 **「终端(管理员)」**
2. 执行：

```powershell
powershell -ExecutionPolicy Bypass -File "<仓库路径>\scripts\wsl_setup.ps1"
```

脚本会做四件事：

| 步骤 | 内容 |
|---|---|
| 0 | 检查 Windows 版本是否支持 WSL2、磁盘是否够用 |
| 1 | 安装 WSL + Ubuntu 22.04 |
| 2 | **写 `.wslconfig` 调整 WSL 资源**（见下方"为什么要调"） |
| 3 | 验证并告诉你下一步 |

> **权限问题**：安装 WSL 需要管理员权限，普通权限的进程无法完成。
> 脚本会自检并在权限不足时给出提示，不会静默失败。

> **可能需要重启**：如果脚本提示需要重启，重启后再跑一次同样的命令。

### 第 2 步：首次进入 Ubuntu，创建账号

```powershell
wsl -d Ubuntu-22.04
```

首次进入会让你创建 Linux 用户名和密码（**密码输入时不显示字符，这是正常的**）。

### 第 3 步：跑构建脚本

在 Ubuntu 里执行（脚本会自己处理源码位置，不会放在 `/mnt/c`）：

```bash
cd /mnt/c/Users/<你的用户名>/Desktop/openvela-superchild
bash scripts/build_openvela.sh
```

然后等它跑完。中途断了不要紧，**重跑会自动续传**。

---

## 为什么要调 `.wslconfig`

WSL2 默认最多只使用物理内存的一半。你的机器 15.3GB → WSL 只拿到约 7.6GB，
而编译 openvela 建议 16GB 以上。不调整的话很可能**中途因 OOM 失败**，
而且报错往往很难指向真正原因。

脚本写入的配置：

```ini
[wsl2]
memory=11GB      # 给 WSL，剩余留给 Windows
swap=16GB        # 内存不够时兜底，强烈建议
processors=12    # 本机 16 逻辑核
```

生效需要 `wsl --shutdown`（会关掉正在运行的 WSL）——脚本会提示你。

---

## 关于「openvela 不支持 WSL」—— 修正后的建议

**我之前的建议偏乐观了，这里更正。**

早先我建议"先花 5 分钟试 WSL"，理由是成本不对等。看到官方 `detect-env.sh` 的
硬判定之后，这个建议需要加一条重要前提：

> **官方检测脚本会明确判 WSL 为 FAIL。这不是误报，是写死在代码里的能力边界。**

所以走 WSL 不是"也许能行"，而是**明知官方判定不通过还要试**。这个决定应该让你知情。

### 三种环境的对比

| 环境 | 官方检测 | 可靠度 | 你的成本 |
|---|---|---|---|
| **VMware / VirtualBox 里的 Ubuntu 22.04** | ✅ 判为「原生 Linux」通过 | 高（厂商教程采用） | 几小时（下载 ISO、图形安装） |
| **裸机 / 双系统 Ubuntu 22.04** | ✅ 通过 | 最高 | 几小时 + 改分区 |
| **云主机 Ubuntu 22.04** | ✅ 通过 | 高 | 约 10 分钟开一台（需花钱，几元/小时） |
| WSL2 | ❌ **明确 FAIL** | 未知 | 5 分钟 + 一次重启 |

### 我的建议

**主路径走虚拟机。** 它是唯一"官方检测通过 + 成本可控"的选择。
你已经开始装的 WSL 可以留着——`build_openvela.sh` 第 0 步会先跑官方检测，
**2 秒内就告诉你环境的真实判定**，不必等同步几十分钟后才发现不行。

**如果 WSL 判定失败但你仍想试**，脚本会提示并要求确认。此时真正该考虑的是
"要不要为一个未知结果投入 1 小时同步 + 编译"，而不只是那 5 分钟安装成本。

### 云主机这条路的可行性

如果你不想在 15.3GB 的笔记本上跑虚拟机（内存会紧张），云主机是个被低估的选项：

- 官方要求是"原生 Linux 上的 Ubuntu 22.04"——**云主机完全满足**
- 不占用本地内存与磁盘，编译还更快
- 国内厂商按量付费，编译一次大约几元到十几元
- 编完把固件下载回本地烧录即可

脚本对 WSL 的防护措施（源码不放 `/mnt/c`、内存检测）对云主机同样适用。

如果所有路径都失败，请把 `~/openvela-build.log` 发出来，里面记录了每一步的完整输出。

---

## 关于 Gitee vs GitHub

构建脚本从 **Gitee** 拉 manifest，不是 GitHub。原因是实测发现：

`openvela.xml` 里的 remote 用的是**相对地址**：

```xml
<remote fetch="../open-vela/" name="openvela"/>
<remote fetch="../" name="git"/>
<default remote="openvela" revision="dev-ai-contest-2026"/>
```

**整个 manifest 里没有任何绝对 URL**。这意味着 `repo sync` 会从"你取 manifest 的那个站"
拉取全部仓库——从 Gitee 起步就全程走 Gitee，国内速度可用。

已验证的事实：

- Gitee 上 `manifests` / `nuttx-apps` / `vendor_allwinnertech` / `packages_ai_agent` 四个仓库都在
- 分支 `dev-ai-contest-2026` 存在
- **匿名克隆可用**（`git ls-remote` 无需登录即成功）

---

## 脚本参数

`build_openvela.sh` 支持环境变量覆盖：

```bash
OPENVELA_SRC=~/my-src bash scripts/build_openvela.sh   # 换源码目录（默认 ~/openvela）
JOBS=4 bash scripts/build_openvela.sh                  # 限制并行度（默认按内存自动推算）
FULL_REBUILD=1 bash scripts/build_openvela.sh          # 先 distclean 全量重建
```

默认**不做 distclean**——只有改过 `menuconfig` 时才需要，否则每次重跑都要多等几十分钟。

---

## 脚本能验证到什么程度（诚实说明）

这两个脚本**没有在真实 WSL 环境里跑过**——因为装 WSL 需要管理员权限，而我没有。

已经做的验证：

| 项 | 状态 |
|---|---|
| `build_openvela.sh` 语法 | ✅ `bash -n` 通过 |
| `wsl_setup.ps1` 语法 | ✅ PowerShell 解析器 0 错误 |
| Gitee 仓库与分支可达 | ✅ 实测确认（含匿名克隆） |
| manifest 用相对地址 | ✅ 实测确认（零绝对 URL） |
| **端到端跑通** | ❌ **未验证** |

所以第一次跑很可能还要改几处（最可能是 `lunch_nuttx` 的菜单编号、
或者某个依赖包名）。脚本在关键位置都给了排查指引，报错信息也尽量指向真正原因。
