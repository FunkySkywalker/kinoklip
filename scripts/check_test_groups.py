#!/usr/bin/env python3
"""Fails when a test is in no ctest group, in two groups, or a group names a test that is gone.

Groups (quick / full / release) are the three lists in tests/groups.cmake; see docs/building.md.
"""
import glob
import os
import re
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
text = open(os.path.join(root, "tests", "groups.cmake")).read()
groups = {}
for var, body in re.findall(r"set\((KK_TESTS_\w+)\s+([^)]*)\)", text):
    groups[var[len("KK_TESTS_"):].lower()] = body.split()

sources = {os.path.basename(f)[:-4]
           for f in glob.glob(os.path.join(root, "tests", "**", "test_*.cpp"), recursive=True)}
# tests registered by CMakeLists.txt itself
extra = set(re.findall(r"add_test\(NAME\s+((?:check|test)_\w+)", open(os.path.join(root, "CMakeLists.txt")).read()))
extra.discard("check_test_groups")  # grouped by hand: always quick
known = sources | {e for e in extra if "${" not in e}

errors = []
seen = {}
for g, names in groups.items():
    for n in names:
        if n in seen:
            errors.append(f"{n} is in both {seen[n]} and {g}")
        seen[n] = g
        if n not in known:
            errors.append(f"{n} is listed in {g} but no such test exists")
for n in sorted(known - set(seen)):
    errors.append(f"{n} has no group: add it to quick, full or release in tests/groups.cmake")
if set(groups) != {"quick", "full", "release"}:
    errors.append("tests/groups.cmake needs exactly the lists quick, full, release")

for e in errors:
    print("error:", e)
sys.exit(1 if errors else 0)
