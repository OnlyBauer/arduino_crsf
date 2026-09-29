#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Turn the host test suites' plain-text output into a JUnit XML report, so
# GitLab's Tests tab shows one row per suite instead of nothing at all.
#
# The suites are deliberately framework-free (see tests/test_util.h): each one
# prints its own failures inline, at the call site, as "  file:line: what: ...",
# and ends with exactly one summary line, "PASS  <name>  <n> checks" or
# "FAIL  <name>  <f>/<n> checks failed" (test_util.h's test_end()). Nothing
# else is printed between one suite's checks and its own summary line, because
# `make -C tests run` runs the suites one at a time and dumps each one's whole
# captured output as a single block before moving to the next.
#
# That is enough structure to report one <testcase> per suite without teaching
# the suites anything about JUnit: buffer lines since the last summary, and
# when a summary line arrives, it owns everything buffered since the one
# before it.
#
# Usage: junit_from_tests.sh <test-output.txt> > junit.xml
# Reads stdin instead if no file is given.

in="${1:-/dev/stdin}"

awk '
  function xml_escape(s) {
    gsub(/&/, "\\&amp;", s)
    gsub(/</, "\\&lt;", s)
    gsub(/>/, "\\&gt;", s)
    gsub(/"/, "\\&quot;", s)
    return s
  }

  BEGIN { pending = ""; n = 0 }

  /^PASS  / || /^FAIL  / {
    name = $2
    is_fail = ($1 == "FAIL")

    names[n] = name
    bodies[n] = pending
    fails[n] = is_fail
    lines[n] = $0
    n++

    pending = ""
    next
  }

  # Every CHECK failure line test_util.h prints starts with at least two
  # literal spaces (and so do a buffer-diffs continuation lines). Nothing
  # else in the suites'"'"' own output does, so this is what tells a suite'"'"'s
  # failure detail apart from build noise (compiler invocations, blank
  # lines) without needing to special-case where in the log it appears.
  /^  / { pending = pending $0 "\n" }

  END {
    total = n
    failed = 0
    for (i = 0; i < n; i++) if (fails[i]) failed++

    print "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    printf "<testsuites tests=\"%d\" failures=\"%d\">\n", total, failed
    printf "  <testsuite name=\"tests\" tests=\"%d\" failures=\"%d\">\n", total, failed
    for (i = 0; i < n; i++) {
      cname = xml_escape(names[i])
      if (fails[i]) {
        printf "    <testcase classname=\"tests\" name=\"%s\">\n", cname
        printf "      <failure message=\"%s\">%s</failure>\n", xml_escape(lines[i]), xml_escape(bodies[i])
        print  "    </testcase>"
      } else {
        printf "    <testcase classname=\"tests\" name=\"%s\" />\n", cname
      }
    }
    print "  </testsuite>"
    print "</testsuites>"
  }
' "$in"
