#!/usr/bin/env awk -f
# THE PROCESS RULES, CHECKED IN ONE PASS -- see test/check_apex_modular.sh section 7.
#
# ⚠️ WHY THIS IS A SEPARATE awk PROGRAM RATHER THAN A LOOP IN THE GATE.
#
# The rules are per-file, and the first version of them was a bash loop that ran six to eight PROGRAMS per
# file (grep, awk, basename, and a `echo | grep` for every name it checked). On this machine a program spawn
# costs ~0.15 s (measured: 29 `basename` calls = 1.3 s), so over twenty scripts the section spent ~11 seconds
# of wall clock doing arithmetic on text. A gate that costs that much to answer a mechanical question is a
# gate that gets skipped -- and the user asked for exactly this: "缩减没必要的反复测试流程".
#
# ⚠️ AND THE RULES MUST NOT DRIFT FROM THE GATE'S DESCRIPTION OF THEM. They are all here, once, and the
# SELF-TESTS in the gate feed this program fixtures whose answers are known (a real call site, a commented
# mention, three launch shapes, a fragment-only call). That is what keeps the file-count check honest: a
# one-pass rewrite of a working check is exactly where a rule quietly stops matching anything.
#
# Input: the script files to check, as arguments. Output: one line per finding, plus ok/fail status per file:
#
#   OWNQUERY <base>              the script executes its own Get-Process query
#   NOCOVER <base> <names...>    the script starts these and nothing in it can stop them
#   NOLIB <base>                 the script kills processes but does not source the shared filter
#   OK <base>                    passed everything that applies to it
#
# A file with no `taskkill` is a reader (several probes are, on purpose), so it is not asked to have a filter;
# a file that starts nothing is not asked to stop anything.

function base(path,   n, i, c) {
  # Portable basename: both separators, no subprocess.
  n = length(path)
  for (i = n; i > 0; i--) {
    c = substr(path, i, 1)
    if (c == "/" || c == "\\") return substr(path, i + 1)
  }
  return path
}

# The three launch shapes, appended to the seen[] array (deduplicated by the caller's loop).
function starts_of(line, seen,   rest, t) {
  rest = line
  while (match(rest, /start "" [A-Za-z0-9._-]+\.exe/)) {
    t = substr(rest, RSTART, RLENGTH)
    sub(/.*start "" /, "", t); sub(/\.exe$/, "", t)
    seen[t] = 1
    rest = substr(rest, RSTART + RLENGTH)
  }
  rest = line
  while (match(rest, /start "" "[^"]+\.exe"/)) {
    t = substr(rest, RSTART, RLENGTH)
    gsub(/"/, "", t); sub(/.*start "" /, "", t); sub(/.*\//, "", t); sub(/.*\\/, "", t); sub(/\.exe$/, "", t)
    seen[t] = 1
    rest = substr(rest, RSTART + RLENGTH)
  }
  rest = line
  while (match(rest, /"[^"]*\.exe" *&/)) {
    t = substr(rest, RSTART, RLENGTH)
    gsub(/"/, "", t); sub(/ *&$/, "", t); sub(/.*\//, "", t); sub(/.*\\/, "", t); sub(/\.exe$/, "", t)
    seen[t] = 1
    rest = substr(rest, RSTART + RLENGTH)
  }
}

# Strip a trailing comment, so a rule is never triggered by prose that explains it.
function code_of(line) {
  sub(/#.*/, "", line)
  return line
}

FILENAME != prev {
  # ⚠️ THE NAME IS REMEMBERED HERE, NOT READ FROM FILENAME INSIDE finish().
  #
  # This rule fires on the FIRST LINE OF A NEW FILE, and at that moment FILENAME is already the NEW file while
  # `lines` still holds the OLD one's text -- so a finish() that read FILENAME would label the previous file's
  # findings with the next file's name. It did: the first version of this program reported check_apex_palette
  # as executing a process query, which is the name that came after a file that did. That is the worst kind of
  # gate bug, because it is entirely convincing.
  if (prev != "") finish(cur_file)
  reset_file()
  prev = FILENAME
  cur_file = FILENAME
}
{ lines[++nlines] = $0 }
END { if (prev != "") finish(cur_file) }

function reset_file() {
  delete starts
  delete killnames
  delete lines
  nlines = 0
  has_taskkill = 0
  has_lib = 0
  own_query = 0
  frag_only = 0
  prevline = ""
}

function finish(path,   i, line, c, missing, name, nstarts, i2, ch, q, d, n, b) {
  for (i = 1; i <= nlines; i++) {
    line = lines[i]

    # -- the launch shapes -------------------------------------------------
    starts_of(line, starts)

    # -- a process query of its own ---------------------------------------
    # The test is the COMMAND FORM: a quoted "Get-Process ..." command string, on a line that mentions
    # powershell or directly after one that does. A regex literal (/Get-Process/) is a pattern, not a call --
    # and a comment has already been stripped by the caller for the rules that need it, but NOT for this one:
    # the comment check happens here because the header of a file that explains the rule is not a breach.
    c = code_of(line)
    if (line !~ /^[[:space:]]*#/ && c ~ /"Get-Process /) {
      if (c ~ /powershell/ || code_of(prevline) ~ /powershell/) own_query = 1
    }

    if (line ~ /taskkill/) has_taskkill = 1
    if (line ~ /lib_procs\.sh/) has_lib = 1

    # -- the names a lib_procs call can stop ------------------------------
    if (c ~ /apex_(kill_own|own_pids)/) {
      killnames[line] = 1
      # A fragment-only call relies on the library's default set (the two host binaries). "Fragment-only"
      # means exactly ONE quoted argument on the line -- the call sites have pipelines and substitutions
      # around them, so counting whitespace tokens reads `| head -1` as a name list and silently drops the
      # check for the very scripts that rely on the default.
      q = 0; d = 0
      n = length(c)
      for (i2 = 1; i2 <= n; i2++) {
        ch = substr(c, i2, 1)
        if (ch == "\x27") q++
        else if (ch == "\x22") d++
      }
      if ((q / 2) + (d / 2) == 1) frag_only = 1
    }

    prevline = line
  }

  b = base(path)
  if (b == "lib_procs.sh" || b == "check_apex_modular.sh") {
    # lib_procs.sh IS the filter; check_apex_modular.sh holds the patterns in order to forbid them. Both are
    # exempt, and both exemptions are bought by the self-tests in the gate -- see the notes there.
    print "OK " b
    return
  }

  if (own_query) print "OWNQUERY " b

  # Coverage: only for scripts that kill something AND start something.
  if (has_taskkill) {
    nstarts = 0; missing = ""
    for (name in starts) {
      nstarts++
      found = 0
      for (line in killnames) if (line ~ ("(^|[^A-Za-z0-9_.-])" name "([^A-Za-z0-9_.-]|$)")) found = 1
      if (found) continue
      if (frag_only && (name == "apex" || name == "apex-settings")) continue
      missing = missing " " name
    }
    if (missing != "") print "NOCOVER " b missing
    else if (nstarts > 0) print "COVEROK " b

    if (!has_lib) print "NOLIB " b
  }
}
