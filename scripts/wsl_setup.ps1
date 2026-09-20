# =============================================================================
#  Windows 侧第一步：安装 WSL2 + Ubuntu 22.04
#
#  用法（必须管理员权限）：
#    1. 右键「开始」→「终端(管理员)」或「Windows PowerShell(管理员)」
#    2. 执行：
#         powershell -ExecutionPolicy Bypass -File "<本文件路径>"
#
#  它会：
#    - 检查 Windows 版本是否支持 WSL2
#    - 安装「虚拟机平台」与「适用于 Linux 的 Windows 子系统」两个功能
#    - 下载并安装 Ubuntu 22.04
#    - 提示是否需要重启，以及重启后该做什么
#
#  为什么必须你自己跑：安装 WSL 需要管理员权限并写入系统功能开关，
#  普通权限的进程无法完成（会直接报「拒绝访问」）。
#
#  ⚠️ 本文件必须保存为「UTF-8 with BOM」！
#     Windows PowerShell 5.1 读取没有 BOM 的 UTF-8 文件时会按系统 ANSI 代码页
#     （简体中文下是 GBK）解码。中文的 UTF-8 字节被当成 GBK 解出来的字符里
#     可能包含引号、反引号，直接把脚本的语法结构破坏掉，
#     报错是「表达式或语句中包含意外的标记"}"」这种看起来莫名其妙的信息。
#     实测踩过这个坑：无 BOM 时有 4 处语法错误，加上 BOM 后 0 错误。
# =============================================================================

# 自检：必须以管理员身份运行
$identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host ""
    Write-Host "  [错误] 当前不是管理员权限，无法安装 WSL。" -ForegroundColor Red
    Write-Host ""
    Write-Host "  请这样做：" -ForegroundColor Yellow
    Write-Host "    1. 右键「开始」按钮"
    Write-Host "    2. 选择「终端(管理员)」"
    Write-Host "    3. 在打开的窗口里执行："
    Write-Host "       powershell -ExecutionPolicy Bypass -File `"$PSCommandPath`"" -ForegroundColor Cyan
    Write-Host ""
    exit 1
}

chcp 65001 | Out-Null
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = "Continue"

Write-Host ""
Write-Host "==============================================================" -ForegroundColor Cyan
Write-Host " 步骤 0 / 系统检查"
Write-Host "==============================================================" -ForegroundColor Cyan

$os = Get-CimInstance Win32_OperatingSystem
$build = [int]$os.BuildNumber
Write-Host "  系统      : $($os.Caption)"
Write-Host "  版本号    : $build"
Write-Host "  内存      : $([math]::Round($os.TotalVisibleMemorySize / 1MB, 1)) GB"

if ($build -lt 19041) {
    Write-Host ""
    Write-Host "  [错误] WSL2 需要 Windows 10 build 19041 以上，当前是 $build。" -ForegroundColor Red
    Write-Host "         请先通过 Windows Update 升级系统。"
    exit 1
}
Write-Host "  WSL2 支持 : 是" -ForegroundColor Green

# 磁盘空间检查：openvela 源码 + 编译产物需要相当空间
$drive = Get-PSDrive C
$freeGB = [math]::Round($drive.Free / 1GB, 1)
Write-Host "  C 盘可用  : $freeGB GB"
if ($freeGB -lt 100) {
    Write-Host ""
    Write-Host "  [警告] openvela 的源码同步 + 编译产物建议预留 80GB 以上。" -ForegroundColor Yellow
    Write-Host "         当前只有 $freeGB GB，可能中途失败。"
    Write-Host "         另外 WSL 的虚拟磁盘默认放在 C 盘（可用 .wslconfig 改到别的盘）。"
    $ans = Read-Host "  仍要继续吗？(y/N)"
    if ($ans -ne "y" -and $ans -ne "Y") { exit 1 }
}

Write-Host ""
Write-Host "==============================================================" -ForegroundColor Cyan
Write-Host " 步骤 1 / 安装 WSL 与 Ubuntu 22.04"
Write-Host "==============================================================" -ForegroundColor Cyan
Write-Host "  这一步会下载约 500MB，可能需要几分钟。请勿关闭窗口。"
Write-Host ""

# --no-launch：装完不立刻启动，避免在这里弹出「输入用户名」把脚本卡住。
# 我们让用户手动 `wsl` 去完成初始化，交互更可控（也方便中途重启）。
& wsl.exe --install -d Ubuntu-22.04 --no-launch
$installRc = $LASTEXITCODE

Write-Host ""
Write-Host "  wsl --install 退出码: $installRc"

# 安装后重新探测一次真实状态，不靠退出码猜
Write-Host ""
Write-Host "==============================================================" -ForegroundColor Cyan
Write-Host " 步骤 2 / 调整 WSL 资源（这一步很关键）"
Write-Host "==============================================================" -ForegroundColor Cyan
Write-Host ""

$totalGB   = [math]::Round($os.TotalVisibleMemorySize / 1MB, 1)
$wslMemGB  = [math]::Floor($totalGB - 4)   # 给 Windows 自己留 4GB
if ($wslMemGB -gt 12) { $wslMemGB = 12 }
if ($wslMemGB -lt 4)  { $wslMemGB = 4 }

Write-Host "  本机总内存 : $totalGB GB"
Write-Host "  WSL2 默认最多只用总内存的一半（约 $([math]::Round($totalGB/2,1)) GB），"
Write-Host "  而编译 openvela 建议 16GB 以上。不调整的话很可能中途 OOM 失败。"
Write-Host ""
Write-Host "  将写入：$env:USERPROFILE\.wslconfig"
Write-Host "      memory     = ${wslMemGB}GB    （给 WSL，其余留给 Windows）"
Write-Host "      swap       = 16GB    （内存不够时兜底，强烈建议）"
Write-Host "      processors = 12      （本机 16 逻辑核）"
Write-Host ""

$wslConfigPath = "$env:USERPROFILE\.wslconfig"
if (Test-Path $wslConfigPath) {
    Write-Host "  [注意] 该文件已存在，为避免覆盖你已有的设置，跳过。" -ForegroundColor Yellow
    Write-Host "         如需手动调整，参考上面三行。" -ForegroundColor Yellow
} else {
    $cfg = @"
[wsl2]
memory=${wslMemGB}GB
swap=16GB
processors=12
"@
    # 必须写成 UTF-8 无 BOM —— .wslconfig 带 BOM 会导致 WSL 解析失败
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($wslConfigPath, $cfg, $enc)
    Write-Host "  已写入 .wslconfig" -ForegroundColor Green
    Write-Host "  需执行 wsl --shutdown 才会生效（会关掉正在运行的 WSL）："
    Write-Host "       wsl --shutdown" -ForegroundColor Cyan
}

Write-Host ""
Write-Host "==============================================================" -ForegroundColor Cyan
Write-Host " 步骤 3 / 验证"
Write-Host "==============================================================" -ForegroundColor Cyan

$distros = (& wsl.exe -l -q) 2>$null | Where-Object { $_ -and $_.Trim() -ne "" }
if ($distros) {
    Write-Host "  已安装的发行版：" -ForegroundColor Green
    $distros | ForEach-Object { Write-Host "    - $($_.Trim())" }
} else {
    Write-Host "  暂未检测到发行版。" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "==============================================================" -ForegroundColor Cyan
Write-Host " 接下来怎么做"
Write-Host "==============================================================" -ForegroundColor Cyan
Write-Host ""

if ($distros) {
    Write-Host "  1) 如果上面列出了 Ubuntu-22.04，说明装好了。" -ForegroundColor Green
    Write-Host "     首次进入需要创建 Linux 用户名与密码："
    Write-Host "       wsl -d Ubuntu-22.04" -ForegroundColor Cyan
    Write-Host "     （密码输入时不显示，正常现象）"
} else {
    Write-Host "  1) 很可能需要重启才能让 WSL 功能生效。" -ForegroundColor Yellow
    Write-Host "     请重启电脑，然后重新运行本脚本一次。" -ForegroundColor Yellow
    Write-Host "     如果仍不行，改用这条命令手动安装发行版："
    Write-Host "       wsl --install -d Ubuntu-22.04" -ForegroundColor Cyan
}

Write-Host ""
Write-Host "  2) 进入 Ubuntu 后，先确认版本是 22.04："
Write-Host "       lsb_release -a" -ForegroundColor Cyan
Write-Host ""
Write-Host "  3) 然后在 Ubuntu 里执行构建脚本（它会自动装依赖、同步源码、编译）："
Write-Host "       cd /mnt/c/Users/<你的用户名>/Desktop/openvela-superchild" -ForegroundColor Cyan
Write-Host "       bash scripts/build_openvela.sh" -ForegroundColor Cyan
Write-Host ""
Write-Host "  注意：源码必须放在 Linux 原生文件系统里（如 ~/openvela），" -ForegroundColor Yellow
Write-Host "        放在 /mnt/c 下会因为跨文件系统而极慢甚至编译失败。" -ForegroundColor Yellow
Write-Host "        构建脚本会自动处理这一点。"
Write-Host ""
