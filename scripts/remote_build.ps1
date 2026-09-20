# =============================================================================
#  在云主机上远程构建 openvela 固件
#
#  用法：
#     powershell -ExecutionPolicy Bypass -File scripts/remote_build.ps1 `
#         -RemoteHost <公网IP> -User ubuntu
#
#  可选参数：
#     -Port 22                       SSH 端口
#     -KeyPath ~\.ssh\id_rsa        私钥路径
#     -SkipBuild                     只做环境检查，不编译
#
#  它做的事：
#     1. 验证 SSH 连通性与主机指纹
#     2. 远端环境检查（Ubuntu 版本 / 内存 / 磁盘 / 免密 sudo）
#     3. 跑官方 detect-env.sh，拿到权威判定
#     4. 上传构建脚本与官方工具
#     5. 执行构建（装依赖 → repo sync → 编译 → 打包）
#     6. 把固件产物取回本地 firmware/
#
#  设计原则：**每一步先验证再继续**。SSH 不通、sudo 要密码、磁盘不够，
#  都在几秒内报出来，而不是等到编译到一半才失败。
# =============================================================================

param(
    [Parameter(Mandatory = $true)][string]$RemoteHost,
    [Parameter(Mandatory = $true)][string]$User,
    [int]$Port = 22,
    [string]$KeyPath = "$env:USERPROFILE\.ssh\id_rsa",
    [switch]$SkipBuild
)

# 刻意用 Continue 而不是 Stop：
# Stop 模式下，原生命令（ssh/scp）的 stderr 经 2>&1 重定向后会被包装成
# ErrorRecord 并**误判为终止性错误**，导致把正常的警告输出也当成失败。
# 本脚本所有关键步骤都显式检查 $LASTEXITCODE，不依赖这个开关来兜底。
$ErrorActionPreference = "Continue"
chcp 65001 | Out-Null
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Split-Path -Parent $ScriptDir
$FwDir     = Join-Path $RepoRoot "firmware"

function Say  { param($m) Write-Host $m }
function Ok   { param($m) Write-Host "  [OK] $m"   -ForegroundColor Green }
function Warn { param($m) Write-Host "  [!!] $m"   -ForegroundColor Yellow }
function Fail { param($m) Write-Host "  [XX] $m"   -ForegroundColor Red }
function Step { param($m) Write-Host ""; Write-Host ("=" * 66) -ForegroundColor Cyan
                           Write-Host " $m" -ForegroundColor Cyan
                           Write-Host ("=" * 66) -ForegroundColor Cyan }

# ---------------------------------------------------------------------------
# 前置检查
# ---------------------------------------------------------------------------

Step "前置检查"

if (-not (Test-Path $KeyPath)) {
    Fail "找不到私钥：$KeyPath"
    Say  "     云主机创建时应该导入了这个公钥：$KeyPath.pub"
    exit 1
}
Ok "私钥存在：$KeyPath"

foreach ($t in @("ssh", "scp")) {
    if (-not (Get-Command $t -ErrorAction SilentlyContinue)) {
        Fail "缺少 $t，请确认已安装 OpenSSH 客户端"
        exit 1
    }
}
Ok "ssh / scp 可用"

$Target = "$User@$RemoteHost"

# 统一的 SSH 选项：
#   BatchMode=yes          —— 不允许交互式提示。密码/口令缺失时**立刻失败**，
#                             而不是卡在那里等输入（远程脚本最怕静默挂住）
#   StrictHostKeyChecking  —— accept-new：首次自动接受指纹并记录，之后校验
#                             变了会报错。比直接关闭校验安全，比手工确认省事
$SshOpts = @(
    "-o", "BatchMode=yes",
    "-o", "StrictHostKeyChecking=accept-new",
    "-o", "ConnectTimeout=15",
    "-p", "$Port",
    "-i", "$KeyPath"
)
$ScpOpts = @(
    "-o", "BatchMode=yes",
    "-o", "StrictHostKeyChecking=accept-new",
    "-o", "ConnectTimeout=15",
    "-P", "$Port",
    "-i", "$KeyPath"
)

function Invoke-Remote {
    param([string]$Command, [switch]$AllowFail)
    $out = & ssh @SshOpts $Target $Command 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -and -not $AllowFail) {
        Fail "远端命令失败（退出码 $LASTEXITCODE）：$Command"
        Say  $out
        exit 1
    }
    return $out
}

# ---------------------------------------------------------------------------
# 1. 连通性
# ---------------------------------------------------------------------------

Step "1 / 连接云主机"

Say "  目标: $Target  端口 $Port"
$banner = & ssh @SshOpts $Target "echo CONNECT_OK; uname -a" 2>&1 | Out-String

if ($banner -notmatch "CONNECT_OK") {
    Fail "SSH 连接失败。逐条排查："
    Say  "     1) 公网 IP 与用户名是否正确（Ubuntu 官方镜像默认用户名是 ubuntu）"
    Say  "     2) 安全组是否放行了 $Port 端口"
    Say  "     3) 创建实例时是否导入了公钥 $KeyPath.pub"
    Say  "     4) 实例是否已经开机"
    Say  ""
    Say  "  原始输出："
    Say  $banner
    exit 1
}
Ok "连接成功"
Say ("     " + (($banner -split "`n" | Where-Object { $_ -match "Linux" } | Select-Object -First 1)).Trim())

# ---------------------------------------------------------------------------
# 2. 远端环境检查
# ---------------------------------------------------------------------------

Step "2 / 远端环境检查"

$envInfo = Invoke-Remote @'
set +e
echo "OS=$( (. /etc/os-release; echo $PRETTY_NAME) )"
echo "VER=$( (. /etc/os-release; echo $VERSION_ID) )"
echo "ARCH=$(uname -m)"
echo "MEM_MB=$(free -m | awk 'NR==2{print $2}')"
echo "DISK_GB=$(df -BG $HOME | awk 'NR==2{print $4}' | tr -d G)"
echo "SWAP_MB=$(free -m | awk '/Swap:/{print $2}')"
if sudo -n true 2>/dev/null; then echo "SUDO=nopasswd"; else echo "SUDO=needs_password"; fi
'@

$info = @{}
foreach ($line in ($envInfo -split "`n")) {
    if ($line -match "^([A-Z_]+)=(.*)$") { $info[$Matches[1]] = $Matches[2].Trim() }
}

Say "  操作系统 : $($info['OS'])"
Say "  版本号   : $($info['VER'])"
Say "  架构     : $($info['ARCH'])"
Say "  内存     : $($info['MEM_MB']) MB"
Say "  磁盘可用 : $($info['DISK_GB']) GB"
Say "  交换空间 : $($info['SWAP_MB']) MB"
Say ""

$blocker = $false

if ($info['OS'] -notmatch "Ubuntu") {
    Fail "远端不是 Ubuntu（$($info['OS'])）。openvela 要求 Ubuntu 22.04。"
    $blocker = $true
} elseif ($info['VER'] -ne "22.04") {
    Warn "远端是 Ubuntu $($info['VER'])，官方要求 22.04。可能能编，但出问题优先怀疑版本。"
} else {
    Ok "Ubuntu 22.04"
}

if ($info['ARCH'] -ne "x86_64") {
    Warn "架构是 $($info['ARCH'])，官方要求 x86_64"
} else {
    Ok "架构 x86_64"
}

$memMB = [int]($info['MEM_MB'])
if     ($memMB -ge 15360) { Ok "内存 $memMB MB（达到官方 16GB 标准）" }
elseif ($memMB -ge 7680)  { Warn "内存 $memMB MB（官方仅警告，但建议加 swap）" }
else                      { Fail "内存 $memMB MB，低于官方 8GB 最低要求"; $blocker = $true }

$diskGB = [int]($info['DISK_GB'])
if     ($diskGB -ge 60) { Ok "磁盘 $diskGB GB" }
elseif ($diskGB -ge 40) { Ok "磁盘 $diskGB GB（达到官方最低 40GB）" }
else                    { Fail "磁盘只有 $diskGB GB，低于官方 40GB 最低要求"; $blocker = $true }

if ($info['SUDO'] -eq "nopasswd") {
    Ok "sudo 免密可用"
} else {
    # 这一条必须提前拦住：编译脚本要装依赖，sudo 若需密码会在无人值守时挂住
    Fail "sudo 需要密码 —— 远程脚本无法输入密码，构建会卡住。"
    Say  "     解决（在云主机上执行一次）："
    Say  "       echo `"$User ALL=(ALL) NOPASSWD:ALL`" | sudo tee /etc/sudoers.d/99-nopasswd"
    Say  "       sudo chmod 440 /etc/sudoers.d/99-nopasswd"
    $blocker = $true
}

if ($blocker) {
    Step "中止"
    Fail "存在必须先解决的问题（见上）。修好后重新运行本脚本。"
    exit 1
}

# ---------------------------------------------------------------------------
# 3. 官方环境检测
# ---------------------------------------------------------------------------

Step "3 / 官方环境检测（detect-env.sh）"

$remoteHome = (Invoke-Remote 'echo $HOME').Trim()
$remoteRoot = "$remoteHome/openvela-build"
Invoke-Remote "mkdir -p $remoteRoot/official" | Out-Null

& scp @ScpOpts "$ScriptDir\official\detect-env.sh" "${Target}:$remoteRoot/official/" | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "上传 detect-env.sh 失败"; exit 1 }

$detect = & ssh @SshOpts $Target "bash $remoteRoot/official/detect-env.sh" 2>&1 | Out-String
Say $detect

if ($detect -match "WSL 环境" -or $detect -match "Docker 环境") {
    Fail "官方检测判定该环境不支持编译。云主机不应出现这一项——请确认买的是纯净 Ubuntu 镜像。"
    exit 1
}
Ok "官方检测未发现阻断项"

if ($SkipBuild) {
    Step "已按 -SkipBuild 停止"
    exit 0
}

# ---------------------------------------------------------------------------
# 4. 上传构建文件
# ---------------------------------------------------------------------------

Step "4 / 上传构建脚本"

Invoke-Remote "mkdir -p $remoteRoot/scripts/official" | Out-Null

& scp @ScpOpts "$ScriptDir\build_openvela.sh" "${Target}:$remoteRoot/scripts/" | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "上传 build_openvela.sh 失败"; exit 1 }

foreach ($f in @("detect-env.sh", "install-deps.sh", "init-repo.sh")) {
    $src = Join-Path $ScriptDir "official\$f"
    if (Test-Path $src) {
        & scp @ScpOpts $src "${Target}:$remoteRoot/official/" | Out-Null
    }
}
Ok "构建脚本已就位：$remoteRoot/scripts/build_openvela.sh"

# ---------------------------------------------------------------------------
# 5. 执行构建
# ---------------------------------------------------------------------------

Step "5 / 执行构建（这一步最慢，中途断了重跑本脚本即可续传）"

Say "  在云主机上运行 build_openvela.sh ..."
Say "  源码会落在 $remoteHome/openvela（Linux 原生文件系统，不经 /mnt）"
Say ""

# 不重定向输出：让它直接流到控制台，以便实时看到进度。
# 注意这里**不加 -t**（不分配 TTY）：构建过程不需要终端，
# 而 TTY 会混入控制字符、让日志难读。
& ssh @SshOpts $Target "cd $remoteRoot && bash scripts/build_openvela.sh 2>&1 | tee ~/openvela-build.log"

if ($LASTEXITCODE -ne 0) {
    Step "构建失败"
    Fail "构建没有正常结束。请把上面最后一个 error（不是最后一个 warning）发出来。"
    Say  "     远端完整日志：$remoteHome/openvela-build.log"
    Say  "     取回日志：  scp -i `"$KeyPath`" ${Target}:$remoteHome/openvela-build.log ."
    exit 1
}
Ok "构建流程结束"

# ---------------------------------------------------------------------------
# 6. 取回固件
# ---------------------------------------------------------------------------

Step "6 / 取回固件产物"

New-Item -ItemType Directory -Force -Path $FwDir | Out-Null

Say "  在远端查找近期生成的镜像文件..."
$found = & ssh @SshOpts $Target @'
find $HOME/openvela/vendor/allwinnertech/lichee -maxdepth 5 -type f \
     \( -name "*.img" -o -name "*.fex" -o -name "*.bin" \) \
     -newermt "-6 hours" -printf "%p\n" 2>/dev/null | head -30
echo "---LOG---"
ls -la $HOME/openvela-build.log 2>/dev/null | awk '{print $5, $9}'
'@

$files = @()
$logLine = ""
$inLog = $false
foreach ($line in ($found -split "`n")) {
    if ($line -match "^---LOG---") { $inLog = $true; continue }
    if ($inLog) { if ($line.Trim()) { $logLine = $line.Trim() } }
    elseif ($line.Trim() -and $line -notmatch "^\s*$") { $files += $line.Trim() }
}

if ($files.Count -eq 0) {
    Warn "没找到近期生成的镜像文件。可能产物命名不同，或打包步骤未产出。"
    Say  "     建议查看远端 lichee 目录下的 out/ 与 tools/ 子目录。"
} else {
    Say "  找到 $($files.Count) 个文件，开始下载："
    foreach ($f in $files) {
        $name = Split-Path $f -Leaf
        Say  "    - $name"
        & scp @ScpOpts "${Target}:$f" "$FwDir\" | Out-Null
        if ($LASTEXITCODE -eq 0) { Ok "已下载到 firmware\$name" } else { Warn "下载失败：$name" }
    }
}

# 日志也一并取回，便于事后复盘
& scp @ScpOpts "${Target}:$remoteHome/openvela-build.log" "$FwDir\openvela-build.log" 2>$null | Out-Null
if ($LASTEXITCODE -eq 0) { Ok "构建日志已取回：firmware\openvela-build.log" }

Step "完成"

Say "  固件目录：$FwDir"
Say ""
Say "  下一步（必须人工）："
Say "    1. 用 PhoenixSuit 烧录 firmware\\ 下的镜像"
Say "    2. 烧录时需要按住板子上的 FEL / RST 键"
Say "    3. 烧完用 ADB 验证："
Say "         adb shell `"uname -a`""
Say "         adb shell `"ls /dev/audio`""
Say "         adb shell `"amixer`""
Say ""
Say "  别忘了：云主机是按量计费的，确认固件已下载后请释放实例。"
Say ""
