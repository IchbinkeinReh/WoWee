#!/usr/bin/env python3
"""Compile the !HAVE_UNICORN branch of the Warden emulator.

    tools/unicorn_stub_check.py [build-dir]

WHY THIS EXISTS

Unicorn Engine is optional. When it is missing, warden_emulator.cpp and
warden_module.cpp compile a stub branch instead - constructors, and one
short definition per method that answers false or zero.

Nothing builds that branch. Every CI leg installs libunicorn: apt on
Ubuntu, brew on macOS, mingw on Windows. So does every developer who
followed the setup instructions. The branch is written once, compiles
nowhere, and rots in place with no test failing and no warning printed.

It rotted. Issue #115: the stub defined WardenEmulator::getRegister,
which the header does not declare and nothing calls, so every build
without Unicorn stopped with "out-of-line definition does not match any
declaration". It was reported from outside, because inside the project
there was no configuration that could see it.

HOW

Take the real compile command for each translation unit that has
-DHAVE_UNICORN, drop the define, and ask the compiler to parse it.

The precompiled header has to go too. It was built with the define, so
leaving it in feeds the compiler the state it would have had and the
mismatch never surfaces - the first attempt at reproducing #115 passed
for exactly that reason.

IT DID NOT RUN ON WINDOWS UNTIL 2026-11-01, TWICE OVER

First the command line: shlex.split in its default POSIX mode reads a
backslash as an escape, and compile_commands.json writes one string per
command with real Windows paths in it, so the compiler's own path came back
without its separators and the subprocess died in CreateProcess. Then, with
that fixed, both translation units reported a failure with an empty error
block: an MSYS2 c++ invoked by absolute path from a shell whose PATH does not
carry its bin directory exits 1 without printing a single word, because cc1plus
never starts. The driver's directory now goes onto the child's PATH.

Nothing caught either one, because no CI leg and no guard entry ran this tool.
The failure was a traceback and a pair of blank "does not compile" blocks, and
both of those read as the sweep having done its job.

WHAT IT CANNOT SEE

Whether the stubs are honest. Each one returns false or zero, and that
is a claim about what the client does without an emulator rather than
something a compiler can check.

Nor a break that only a different compiler reports. This runs whichever
one built the tree; #115 was found by GCC and reproduced here by clang,
but that is not guaranteed for the next one.
"""
import json
import os
import pathlib
import shlex
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFINE = "HAVE_UNICORN"


def compile_db(build_dir):
    """The compile_commands.json for this tree, or None with a reason."""
    candidates = []
    if build_dir:
        candidates.append(pathlib.Path(build_dir) / "compile_commands.json")
    candidates.append(ROOT / "build" / "compile_commands.json")
    for path in candidates:
        if path.is_file():
            return path, None
    return None, "no compile_commands.json in " + ", ".join(
        str(p.parent) for p in candidates)


def split(command):
    """The command line cut into arguments - POSIX-style only where the shell
    is POSIX.

    `shlex.split`'s default treats a backslash as an escape, and
    compile_commands.json writes the command as one string with real Windows
    paths in it. The compiler's own path came back as
    `C:Usersichbimsys64mingw64binc++.exe`, and the subprocess died in
    CreateProcess with "the system cannot find the file specified" on a tree
    that is configured perfectly well. So this sweep has never run on Windows:
    it raised before compiling anything, and nothing noticed, because nothing
    ran it.

    posix=False keeps the quotes that grouping the argument needs, which a
    quoted path does not want, so they come back off.
    """
    if os.name == "nt":
        return [a.strip('"\'') for a in shlex.split(command, posix=False)]
    return shlex.split(command)


def without_unicorn(entry):
    """The entry's command, minus the define, the PCH, and the output file."""
    args = split(entry.get("command") or " ".join(entry["arguments"]))
    out, i = [], 0
    while i < len(args):
        arg = args[i]
        if arg in ("-include-pch", "-o"):
            i += 2
            continue
        if arg == "-Xclang" and i + 1 < len(args) and args[i + 1] == "-include-pch":
            i += 4  # -Xclang -include-pch -Xclang <file>
            continue
        if arg == "-Winvalid-pch":
            i += 1
            continue
        if arg.startswith("-D" + DEFINE):
            i += 1
            continue
        if arg == "-c":
            out.append("-fsyntax-only")
            i += 1
            continue
        out.append(arg)
        i += 1
    return out


def main():
    build_dir = sys.argv[1] if len(sys.argv) > 1 else None
    path, why = compile_db(build_dir)
    if path is None:
        print("unicorn_stub_check: " + why)
        print("  Configure with cmake first; CMAKE_EXPORT_COMPILE_COMMANDS is")
        print("  on unconditionally, so a configured tree always has one.")
        return 1

    # The define is set on the whole wowee target, so it is on the command
    # line of every translation unit in it. Only the two that name it have a
    # branch to compile; checking the other 413 costs minutes and proves
    # nothing.
    entries, seen = [], set()
    for entry in json.load(open(path)):
        command = entry.get("command") or " ".join(entry["arguments"])
        if "-D" + DEFINE not in command:
            continue
        source = pathlib.Path(entry["file"])
        if source in seen or not source.is_file():
            continue
        if DEFINE not in source.read_text(encoding="utf-8", errors="ignore"):
            continue
        seen.add(source)
        entries.append(entry)

    if not entries:
        # Unicorn was not found when this tree was configured, so the stub
        # branch is what the build already compiles. Nothing left to check -
        # and said in the same shape as the answer above, because a sweep that
        # reports nothing at all cannot be told apart from one that broke.
        print("unicorn_stub_check: 0 translation unit(s) compile without "
              "Unicorn - this tree already builds without it")
        return 0

    failed = []
    for entry in entries:
        name = pathlib.Path(entry["file"]).name
        cmd = without_unicorn(entry)
        # The compiler's own directory goes onto PATH for the child.
        #
        # On MSYS2 a c++ invoked by absolute path with its bin directory not on
        # PATH exits 1 having printed *nothing at all* - cc1plus never starts,
        # and there is no diagnostic to read. Every compile here names
        # C:\...\msys64\mingw64\bin\c++.exe absolutely, and the tool inherits a
        # shell whose PATH does not carry it, so both translation units came
        # back as failures with an empty error block - the opposite of what this
        # sweep is for, which is showing the error the stub branch hides.
        env = None
        driver = pathlib.Path(cmd[0])
        if driver.parent.is_dir():
            env = dict(os.environ, PATH=os.pathsep.join(
                [str(driver.parent), os.environ.get("PATH", "")]))
        # errors="replace": a compiler is free to quote a source line back at
        # us, and the Warden module tables hold raw bytes.
        result = subprocess.run(cmd, capture_output=True, env=env,
                                encoding="utf-8", errors="replace")
        if result.returncode != 0:
            # Said out loud rather than left as a blank block: a failure with
            # no diagnostic is a different animal from a broken stub, and it is
            # the one this tool walked into.
            failed.append((name, result.stderr.strip() or
                           "(the compiler exited %d without printing anything"
                           % result.returncode))
        else:
            print("  ok  " + name)

    if failed:
        print()
        print("The Warden stubs do not compile without Unicorn:")
        for name, stderr in failed:
            print()
            print("--- " + name + " ---")
            print(stderr)
        print()
        print("These are the definitions under #else // !HAVE_UNICORN. They")
        print("have to match the header the same way the real ones do.")
        return 1

    print("unicorn_stub_check: %d translation unit(s) compile without Unicorn"
          % len(entries))
    return 0


if __name__ == "__main__":
    sys.exit(main())
