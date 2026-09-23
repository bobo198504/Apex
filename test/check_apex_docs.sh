#!/usr/bin/env bash
# =============================================================================
# AGENTS.md 还读得完吗 -- 一条会自己长大的规矩，以及它的机械判据。
#
# WHY THIS GATE EXISTS (measured, 2026-09-20):
#   AGENTS.md had grown to 126,676 bytes / 1,474 lines. A session can take in roughly 65 KB of instructions,
#   and the harness says so out loud: "truncated AGENTS.md from 126676 to 65243 bytes". What got dropped was
#   everything from about 60% of the way down -- including 绝对红线, 门, 已知的坑, 变更纪律. The rules that
#   matter most were the ones nobody was reading any more.
#
#   The failure mode is a feedback loop, which is why a gate is the right answer and a resolution is not:
#   every lesson added a line -> more lines meant less was read -> less read meant the same mistakes came back
#   -> which added more lines. Nothing in the suite could see any of it, because a document that is too long
#   does not fail -- it just gets quiet.
#
# WHAT IT CHECKS (four things, all mechanical):
#   1. AGENTS.md is under the budget. The whole point: a file over the budget is a file half of which does not
#      exist as far as the next session is concerned.
#   2. Every topic file the index names actually exists.
#   3. Every .md in docs/rules/ is named by the index -- no orphan topic files (a topic nobody is pointed at
#      is a topic nobody reads, which is the same failure one level down).
#   4. The landmark sections are still somewhere in the corpus (root file OR a topic file). This is the
#      "nothing was thrown away in the move" half: the split moved ~1,300 lines, and the way that goes wrong
#      is silently, by not copying something.
#
# ⚠️ IT DOES NOT CHECK THAT THE TOPIC FILES ARE *GOOD*. It cannot: quality is not mechanical. What it can do is
# make "I can no longer read all of this" loud instead of silent, which is the only part that was actually
# killing the edit loop.
# =============================================================================
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"

ROOT_DOC="$ROOT/AGENTS.md"
TOPIC_DIR="$ROOT/docs/rules"

# 48 KB, not 64 KB: the budget also has to hold every other instruction a session carries (the harness's own
# rules, the tool descriptions), so the project's own file must leave room. Measured headroom is the point --
# a file that exactly fills the budget is one paragraph away from being truncated again.
BUDGET=49152

fail=0
say()  { printf '%s\n' "$*"; }
bad()  { say "   FAIL: $*"; fail=1; }

if [ ! -f "$ROOT_DOC" ]; then
  say "   FAIL: AGENTS.md is missing"
  exit 1
fi

# ---- 1. the budget ----------------------------------------------------------
root_bytes=$(wc -c < "$ROOT_DOC" | tr -d ' ')
root_lines=$(wc -l < "$ROOT_DOC" | tr -d ' ')
if [ "$root_bytes" -le "$BUDGET" ]; then
  say "   AGENTS.md is $root_bytes bytes / $root_lines lines (budget $BUDGET)"
else
  bad "AGENTS.md is $root_bytes bytes -- over the $BUDGET budget by $((root_bytes - BUDGET)).
        A file over the budget is only PARTLY read: the rest silently does not exist for the next session.
        Move the narrative sections into docs/rules/ and leave a one-line rule plus an index row here."
fi

# ---- 2/3. the index and the topic files agree, in both directions -----------
mkdir -p "$TOPIC_DIR"
listed=$(sed -n 's/.*\(docs\/rules\/[A-Za-z0-9._-]*\.md\).*/\1/p' "$ROOT_DOC" | sort -u)
if [ -z "$listed" ]; then
  bad "the index in AGENTS.md names no docs/rules/*.md file at all"
fi

for rel in $listed; do
  if [ -f "$ROOT/$rel" ]; then
    say "   indexed: $rel  ($(wc -c < "$ROOT/$rel" | tr -d ' ') bytes)"
  else
    bad "$rel is named by the index but does not exist"
  fi
done

# The other direction. Every topic file must be reachable from the root document, or it is a file nobody
# will ever open -- the same "written but never read" failure this gate is about, one level down.
for f in "$TOPIC_DIR"/*.md; do
  [ -f "$f" ] || continue
  base="docs/rules/${f##*/}"
  case "$listed" in
    *"$base"*) ;;
    *) bad "$base exists but no index row in AGENTS.md points at it (a topic nobody is sent to is a topic nobody reads)" ;;
  esac
  # And each topic file has to say WHEN to read it -- that line is what makes the index useful.
  if head -8 "$f" | grep -q '何时读我'; then
    :
  else
    bad "$base has no '何时读我' line in its first lines"
  fi
done

# ---- 4. nothing was lost in the move ---------------------------------------
# One landmark per section that moved out. If a future edit deletes a topic file, or copies one badly, the
# section it carried stops existing somewhere and this says so. The check is deliberately shape-based: it
# asks whether the LANDMARK is still reachable, not where it lives.
landmarks=(
  "绝对红线"
  "已知的坑"
  "更新日志格式"
  "面板页面已踩过的坑"
  "抓钩与注入的五条硬约束"
  "加一个新功能"
  "语言与主题"
  "三个模型"
  "设置面板是"
  "版本记录"
)
# One pass over the whole corpus (root + topics), not one grep per landmark: spawning a process per landmark
# is how a "fast" gate becomes a slow one (see the cost notes in docs/rules/gates.md).
corpus=$(cat "$ROOT_DOC" "$TOPIC_DIR"/*.md 2>/dev/null)
missing=()
for lm in "${landmarks[@]}"; do
  case "$corpus" in
    *"$lm"*) ;;
    *) missing+=("$lm") ;;
  esac
done
if [ ${#missing[@]} -eq 0 ]; then
  say "   all ${#landmarks[@]} landmark sections are still reachable somewhere in AGENTS.md + docs/rules/"
else
  bad "these sections cannot be found anywhere: ${missing[*]}
        (the split moved text between files; this is what catches a section that did not survive the move)"
fi

say ""
if [ "$fail" -eq 0 ]; then
  say "OK: the rules are still short enough to be read whole"
fi
exit $(( fail > 0 ? 1 : 0 ))
