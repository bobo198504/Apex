#!/usr/bin/env bash
# =============================================================================
# 编辑循环到底花在哪：量一次，别猜。（这不是门，不会让任何东西变红；它只回答"为什么这次这么慢"。）
#
# 起因：用户的感觉是"刚开始开发挺快，功能越多，单次小改动越久"。感觉可能是对的，也可能是错的，
# 所以要量。第一次量出来的账（2026-09-20）：
#
#   bash apex/build.sh  什么都没改      3,211 ms   <-- 两个 exe 每次都重新链接（见下）
#   改一个 .cpp 之后                     5,522 ms
#   改 AutoIME（Rust）之后               6,479 ms
#   改 panel.html 之后                   3,515 ms
#   15 扇构建层门，逐扇相加             41,929 ms
#   整层一次跑完（run_all.sh）           37,936 ms   <-- 比逐扇相加还快：进程与文件缓存热了
#   其中 check_apex_modular             13,657 ms
#        check_apex_deploy               6,943 ms
#        check_feature_keepawake         5,533 ms
#        check_feature_ime               5,080 ms
#   cargo 空跑一次                          61 ms   <-- 它从来不是"构建慢"的原因
#
# 后来加了第 16 扇门（check_apex_docs，约 1.6 s），重建后同一批数字是：构建 3.4 s、逐扇相加 41.7 s、
# 整层 38.6 s。**数字会漂**——要当前的数就跑本脚本，别抄这里。
#
# 判据：编辑循环 = 构建 + 门，而**门是大头**。这就是"功能越多越慢"的来源——门是随功能一条条加上来的，
# 而"跑哪些"还没有选择机制。（候选做法记在 docs/rules/build-loop.md。）
#
# ⚠️ 本脚本会 `touch` 三个源文件来量"改一个文件要多久"，所以它**改动了 mtime**：跑完之后
# `build.sh` 会重新编译这三处。它不写任何产物的内容，也不碰正在运行的 Apex。
# =============================================================================
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
cd "$ROOT" || exit 1

# Git bash has no `bc`; milliseconds come from GNU `date +%s%3N` and integer arithmetic.
now() { date +%s%3N; }
ms()  { echo $(( $2 - $1 )); }
show(){ printf "   %7d ms  %s\n" "$1" "$2"; }

echo "=== 构建：什么都没改 ==="
s=$(now); bash apex/build.sh >/dev/null 2>&1; e=$(now); show "$(ms "$s" "$e")" "bash apex/build.sh"
echo "   (⚠️ 两个 exe 每次都重新链接 —— 见 gates.md / build-loop.md；这就是它不为 0 的原因)"

echo
echo "=== 构建：改一个文件（touch 只改时间戳，不改内容） ==="
for f in apex/main.cpp features/AutoIME/src/settings.rs apex/ui/panel.js; do
  [ -f "$f" ] || continue
  touch "$f"
  s=$(now); bash apex/build.sh >/dev/null 2>&1; e=$(now)
  show "$(ms "$s" "$e")" "改 $f 之后"
done

echo
echo "=== 门：逐扇计时（**只跑构建层**） ==="
# ⚠️ THE LIST STOPS WHERE THE BUILD LAYER STOPS. `run_all.sh` names every gate in one file, and half of them
# are the ASSEMBLY layer -- they start the real program, and several take the user's Apex away and give it
# back. Reading all of them made this tool (a) take two and a half minutes and (b) stop the user's program
# while only measuring. The first version did exactly that. The boundary is the `if [ "$ASSEMBLY" != "1" ]`
# line, so that is where the scan stops.
total=0
for g in $(awk '/^if \[ "\$ASSEMBLY"/{exit} /^run_gate/{print $2}' test/run_all.sh); do
  script="test/$g.sh"
  [ -f "$script" ] || continue
  s=$(now); bash "$script" >/dev/null 2>&1; rc=$?; e=$(now)
  d=$(ms "$s" "$e"); total=$(( total + d ))
  printf "   %7d ms  rc=%d  %s\n" "$d" "$rc" "$g"
done
printf "   -------\n   %7d ms  构建层逐扇相加\n" "$total"

# ⚠️ `--assembly` MEASURES THE OTHER HALF TOO, AND THAT IS WHERE THE TIME ACTUALLY GOES: a delivery is about
# 258 s and the build layer is only ~38 s of it. This option DOES start the real program (that is what the
# assembly layer is) and takes the field once, so the user's Apex blinks once -- but with APEX_NO_TRAY the
# scratch copies themselves stay off the screen.
if [ "${1:-}" = "--assembly" ]; then
  echo
  echo "=== 门：装配层逐扇计时（起真程序；清场一次） ==="
  atotal=0
  for g in $(awk '/^if \[ "\$ASSEMBLY"/{f=1;next} f && /^run_gate_maybe/{print $2}' test/run_all.sh); do
    script="test/$g.sh"
    [ -f "$script" ] || continue
    s=$(now); bash "$script" >/dev/null 2>&1; rc=$?; e=$(now)
    d=$(ms "$s" "$e"); atotal=$(( atotal + d ))
    printf "   %7d ms  rc=%d  %s\n" "$d" "$rc" "$g"
  done
  printf "   -------\n   %7d ms  装配层逐扇相加\n" "$atotal"
  printf "   %7d ms  两层合计（逐扇相加）\n" "$(( total + atotal ))"
fi

echo
echo "=== 门：整层一次跑完（编辑循环真正的那一条命令） ==="
s=$(now); bash test/run_all.sh >/dev/null 2>&1; rc=$?; e=$(now)
printf "   %7d ms  rc=%d\n" "$(ms "$s" "$e")" "$rc"

echo
echo "（本脚本的参数：无。要看门的输出就跑 run_all.sh；它只负责把时间摆出来。）"
