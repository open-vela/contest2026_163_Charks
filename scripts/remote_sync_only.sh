#!/usr/bin/env bash
#
# remote_sync_only.sh —— **只做源码同步**，不编译不打包。
#
# 为什么需要它
# ------------
# build_openvela.sh 是一口气跑完「自检 → sync → 打补丁 → 编译 → 打包」的。
# 但 R5 要在编译前插两件事：
#
#   1. 就位 speexdsp 源码（它从 GitHub 下载，国内云主机等于不通）
#   2. 开启 AI Agent 并接入 board/baidu_voice，这些都要先看到源码才能配
#
# 如果让 build_openvela.sh 直接跑到底，就得先编一轮"没带 AI Agent"的固件，
# 然后再编第二轮 —— 白花一次完整编译的时间。
#
# 所以把同步单独拆出来先跑完，配置好了再让 build_openvela.sh 接手：
# 它发现 .repo 已存在会跳过 init，sync 也几乎是空转，直接进编译。
#
# 用法（远端）：
#     setsid nohup bash /root/openvela-build/scripts/remote_sync_only.sh \
#         > /root/sync.out 2>&1 < /dev/null &
#     tail -f /root/sync.out
#
# 参数与 build_openvela.sh 保持一致，改一处即可。

set -uo pipefail

MANIFEST_URL="https://gitee.com/open-vela/manifests.git"
MANIFEST_BRANCH="dev-ai-contest-2026"
MANIFEST_FILE="openvela.xml"

SRC_ROOT="${OPENVELA_SRC:-$HOME/openvela}"

log()  { echo "[$(date +%H:%M:%S)] $*"; }
ok()   { echo "[$(date +%H:%M:%S)] ✓ $*"; }
die()  { echo "[$(date +%H:%M:%S)] ✗ $*" >&2; exit 1; }

mkdir -p "$SRC_ROOT"
cd "$SRC_ROOT" || die "无法进入 $SRC_ROOT"

# ---------------------------------------------------------------------------
# 1. git 全局配置
# ---------------------------------------------------------------------------
log "配置 git"
git config --global http.postBuffer 524288000 || true
git config --global http.lowSpeedLimit 0        || true
git config --global http.lowSpeedTime 999999    || true
git config --global core.compression 0          || true

# git-lfs 的全局过滤器必须在这里**再确认一次**：
# repo sync 内部走普通 git clone，靠的就是这套全局 filter.lfs.* 配置
# 来把 LFS 指针替换成实体文件。缺了它，拉到的就是 134 字节的指针
# —— 症状要等到链接阶段才暴露，非常难查。
if ! git lfs version >/dev/null 2>&1; then
    die "git-lfs 未安装。先跑 scripts/install_pack_deps.sh 或 apt-get install -y git-lfs"
fi
git lfs install --skip-repo >/dev/null 2>&1 || true
ok "git-lfs 就绪：$(git lfs version)"

# ---------------------------------------------------------------------------
# 2. repo init
# ---------------------------------------------------------------------------
if [[ -d .repo ]]; then
    ok "已存在 .repo，跳过 init"
else
    log "repo init（从 Gitee 拉 manifest）"
    log "  manifest : $MANIFEST_URL"
    log "  分支     : $MANIFEST_BRANCH"
    log "  清单     : $MANIFEST_FILE"

    # --git-lfs 不是所有 repo 版本都支持，按能力探测。
    # 即使不支持，只要上面 git lfs install 过了，smudge 过滤器照样生效。
    LFS_FLAG=()
    if repo init --help 2>&1 | grep -q -- "--git-lfs"; then
        LFS_FLAG=(--git-lfs)
        ok "repo 支持 --git-lfs，已启用"
    else
        log "repo 不支持 --git-lfs，依赖全局 filter.lfs 配置"
    fi

    # 级联尝试：先让 repo 用自带的本地源码；失败再换 --repo-url 镜像。
    # （repo init 默认要去 gerrit.googlesource.com 下 repo 自身，国内不通）
    init_ok=0
    for attempt in "local" "tuna"; do
        case "$attempt" in
            local) extra=() ;;
            tuna)  extra=(--repo-url=https://mirrors.tuna.tsinghua.edu.cn/git/git-repo) ;;
        esac

        log "尝试方式：$attempt"
        if repo init -u "$MANIFEST_URL" -b "$MANIFEST_BRANCH" -m "$MANIFEST_FILE" \
                "${LFS_FLAG[@]}" "${extra[@]}" --no-repo-verify; then
            init_ok=1
            break
        fi
        log "方式 $attempt 失败，换下一种"
    done

    [[ "$init_ok" == "1" ]] || die "repo init 全部方式失败"
    ok "repo init 完成"
fi

# ---------------------------------------------------------------------------
# 3. repo sync
# ---------------------------------------------------------------------------
log "repo sync 开始（31GB，1-2 小时；断了重跑本脚本会续传）"
log "  -c 只同步当前分支 / --no-tags 不拉标签 / -j8 并行"

# -j8 对 4 核机器偏激进但 repo sync 主要受网络限制，实测可行。
# 失败多半是网络中断 —— repo sync 自带断点续传，重跑即可。
if repo sync -c --no-tags -j8; then
    ok "repo sync 完成"
else
    die "repo sync 失败（多为网络中断）。直接重跑本脚本即可续传。"
fi

# ---------------------------------------------------------------------------
# 4. 关键成果确认
# ---------------------------------------------------------------------------
log "校验同步结果"

fail=0
check_dir() {
    if [[ -d "$SRC_ROOT/$1" ]]; then
        ok "存在：$1"
    else
        echo "  ✗ 缺失：$1"
        fail=1
    fi
}
check_dir "vendor/allwinnertech/boards/r528/r528s3-gemini-s1"
check_dir "packages/ai_agent"
check_dir "nuttx"
check_dir "apps"

log "分支确认：$(git -C "$SRC_ROOT/nuttx" rev-parse --abbrev-ref HEAD 2>/dev/null || echo '未知')"

log "源码体积：$(du -sh "$SRC_ROOT" 2>/dev/null | cut -f1)"
log "磁盘剩余：$(df -BG "$SRC_ROOT" | tail -1 | awk '{print $4}')"

if [[ "$fail" == "1" ]]; then
    die "有目录缺失。多半是 manifest 分支不对（应为 $MANIFEST_BRANCH）"
fi

ok "源码同步全部就绪，可以开始配置与编译"
