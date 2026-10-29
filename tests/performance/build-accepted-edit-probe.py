#!/usr/bin/env python3
"""Link the same probe to an already-built checkout's controller libraries."""
import pathlib
import shlex
import subprocess
import sys

if len(sys.argv) != 4:
    sys.exit("usage: build-accepted-edit-probe.py CHECKOUT SOURCE OUTPUT")
root, source, output = map(pathlib.Path, sys.argv[1:])
build_tests = root / "build/desktop/tests"
flags_file = build_tests / "CMakeFiles/branch_truce_coordinator_tests.dir/flags.make"
flags = {}
for line in flags_file.read_text().splitlines():
    if " = " in line:
        key, value = line.split(" = ", 1)
        flags[key] = shlex.split(value)
link_file = build_tests / "CMakeFiles/branch_truce_coordinator_tests.dir/link.txt"
link = shlex.split(link_file.read_text())
libs = []
for word in link:
    if word.endswith(".a") or ".so" in word or "qrc_qmake_BranchTruce_App_init.cpp.o" in word:
        path = pathlib.Path(word)
        libs.append(str(path if path.is_absolute() else build_tests / path))
command = ["c++", *flags["CXX_DEFINES"], *flags["CXX_INCLUDES"], *flags["CXX_FLAGS"],
           str(source), "-o", str(output), *libs]
subprocess.run(command, cwd=build_tests, check=True)
