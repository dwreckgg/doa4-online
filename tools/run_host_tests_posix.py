"""Compiles and runs the host tests that need no Windows header, one after another, with a POSIX compiler line.

  python3 -I tools/run_host_tests_posix.py [--only NAME ...] [--cxx CXX] [--manifest PATH] [--timeout SECONDS]
  python3 -I tools/run_host_tests_posix.py --check-names [--manifest PATH]

The Linux job's runner (.github/workflows/linux-build.yml); tools/run_host_tests.py runs the same tests on Windows
through their run_<name>_test.bat. Which tests run is written in tests/host/posix_manifest.txt, one entry a line:

  <name> | <sources, space separated, relative to the repository root> | <extra flags>     a test that runs
  !<name> | <reason>                                                                     a test left out, with why
  # ...                                                                                  a comment

<name> is the middle part of run_<name>_test.bat. In the flags, {SDK} stands for the SDK folder (the environment's
REXSDK_DIR); nothing else is expanded. A line that uses {SDK} while REXSDK_DIR is unset or not a folder "did not run".
Each test is compiled in the repository root with
  <cxx> -std=c++23 -O2 <flags> <sources> -o <tmp>/<name>
(<tmp> is one fresh temporary folder for the whole run, removed at the end) and its program is started in tests/host
with no input. A program still running after --timeout seconds (default 600), or a compiler still running after
--compile-timeout seconds (default 1800), is stopped with its child processes and the test "did not run". The
program's output (stdout and stderr together) is judged by judge() of tools/run_host_tests.py: the test's own summary
line and its exit code.
Prints one line a test, "<name>: passed N", "<name>: FAILED (...)" or "<name>: did not run (...)" (a compile failure is
"did not run (compile failed; exit N)"), then for a test that did not pass the last 40 lines of its compiler or program
output; a line "left out: <name> (<reason>)" for each left-out test; and as the last line
  posix host tests: <ran> ran, <passed> passed, <left out> left out, manifest <n>
<n> = the manifest's run lines (all of them, also under --only); <ran> = the tests that compiled and whose program
ended by itself within the limit, whatever its output then says.
Exit 0 only when every run line of the manifest ran and passed; with --only, when every named test passed. A manifest
error, a manifest with no run line, or an --only name that is not a run line gives exit 1 before anything is compiled.
--cxx is ONE program name or path (default c++), never split into words. --repo names another repository root and
--exe-suffix the suffix of the program file the compiler is asked to write; both exist for this runner's own tests
(tests/tools/test_release_tools.py), the Linux job passes neither.
--check-names compiles and starts nothing and needs no compiler and no SDK: it reads the manifest as above (an error
there gives the same lines and exit 1) and compares its names, run lines and left-out lines alike, with the files of
tests/host whose whole name is run_<name>_test.bat. It prints one line for each runner that is on no line, then one for
each line that has no runner (each list sorted by name), and as the last line
  posix manifest names: <m> lines, <r> runners, <equal | NOT equal>
Exit 0 only on "equal"; a folder with no runner at all gives one line and exit 1, and so does --check-names with --only.
The Linux job runs it as its first step, so a new run_<name>_test.bat with no manifest line is seen there too.
Two rules a run line must meet, checked by the same tests over the line's sources and the local #include "..." files
they reach: windows_reach(), no Windows header outside a block that is compiled only on Windows; and msvc_calls(), no
call spelled the Microsoft way (_name( or name_s() outside such a block. The second is a net over spellings, not a
proof that the code compiles elsewhere.
"""
import argparse
import importlib.util
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
MANIFEST = "tests/host/posix_manifest.txt"  # relative to the repository root
TAIL = 40
DEFAULT_TIMEOUT = 600  # seconds for each test program
DEFAULT_COMPILE_TIMEOUT = 1800  # seconds for each compile

# The Windows-only headers (#include <...>), lower case. Built from the sources: every angle include in the closures
# of the 71 host tests was placed once (C or C++ standard, POSIX, Linux or compiler, Windows-only, SDK or third party).
# The first three rows are the Windows-only ones found there (tests/tools/test_release_tools.py checks that every
# header found inside a Windows-only block is one of them); the last row is others of the kind, which no test names yet.
WINDOWS_HEADERS = frozenset((
    "windows.h", "winsock2.h", "ws2tcpip.h", "winhttp.h", "bcrypt.h", "iphlpapi.h", "intrin.h", "process.h", "psapi.h",
    "shlobj.h", "shellapi.h", "shobjidl.h", "knownfolders.h", "objbase.h", "wincodec.h", "winioctl.h", "wrl/client.h",
    "mstcpip.h", "wlanapi.h",
    "io.h", "direct.h", "conio.h",
))

# A call spelled the Microsoft way: an identifier that starts with _ and a small letter (_getpid, _dupenv_s, _wfopen)
# or ends in _s (fopen_s, localtime_s), followed by '(' and not a member or a qualified name. The headers above cannot
# show such a call: a source may call _getpid() with <process.h> correctly inside #ifdef _WIN32. Built from the data
# like the header list: over the run lines' closures this spelling occurred once outside a Windows-only block
# (_getpid, tests/host/costumes_host_test.cpp, since put under #ifdef _WIN32).
MSVC_CALL = re.compile(r"(?<![\w.>:])(_[a-z]\w*|\w+_s)\s*\(")
# Names of that spelling that are portable all the same (a project function, a C11 Annex K name with a fallback): none
# today. Add a name here only with a comment that says where it is defined for every platform.
PORTABLE_CALLS = frozenset()


def _load_judge():
    """judge() of tools/run_host_tests.py, loaded by its path (under -I the tools folder is not on sys.path) without
    leaving a tools/__pycache__."""
    before = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec = importlib.util.spec_from_file_location("run_host_tests", os.path.join(HERE, "run_host_tests.py"))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = before
    return module.judge


judge = _load_judge()


# ---- the manifest ---------------------------------------------------------------------------------------------------

def parse_manifest(text):
    """(runs, left_out, errors) of a manifest text: runs = [(name, [sources], [flags])] in file order, left_out =
    [(name, reason)], errors = ['line N: ...'], one for each bad line. A name may be on one line only: a second line
    with it is an error and the first stays. A run line is cut at its first two '|', a left-out line at its first."""
    runs, left_out, errors, seen = [], [], [], {}
    for n, line in enumerate(text.split("\n"), 1):
        line = line.rstrip("\r").strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("!"):
            name, _, reason = (part.strip() for part in line[1:].partition("|"))
            if not re.fullmatch(r"\w+", name):
                errors.append(f"line {n}: a left-out line is '!<name> | <reason>', the name letters, digits and _: {line!r}")
                continue
            if not reason:
                errors.append(f"line {n}: left-out test {name} has no reason: {line!r}")
                continue
            entry = None
        else:
            parts = line.split("|", 2)
            if len(parts) != 3:
                errors.append(f"line {n}: a run line is '<name> | <sources> | <flags>' (both | are there): {line!r}")
                continue
            name, sources, flags = parts[0].strip(), parts[1].split(), parts[2].split()
            if not re.fullmatch(r"\w+", name):
                errors.append(f"line {n}: a run line starts with its name, letters, digits and _: {line!r}")
                continue
            if not sources:
                errors.append(f"line {n}: run line {name} names no source: {line!r}")
                continue
            entry = (name, sources, flags)
        if name in seen:
            errors.append(f"line {n}: {name} is already on line {seen[name]}; a test is on one line only")
            continue
        seen[name] = n
        if entry is None:
            left_out.append((name, reason))
        else:
            runs.append(entry)
    return runs, left_out, errors


RUNNER_BAT = re.compile(r"run_(\w+)_test\.bat")  # a Windows runner's whole file name; <name> is the group


def check_names(repo, manifest, runs, left_out):
    """--check-names: prints how the manifest's names (run lines and left-out lines) differ from the names of the
    run_<name>_test.bat files in <repo>/tests/host, then the count line; the exit code, 0 only when the two sets are
    equal and a runner was found."""
    folder = os.path.join(repo, "tests", "host")
    try:
        files = os.listdir(folder)
    except OSError:
        files = []
    runners = {m.group(1) for m in map(RUNNER_BAT.fullmatch, files) if m}
    if not runners:
        print(f"run_host_tests_posix: no run_<name>_test.bat in {folder}")
        return 1
    names = {name for name, _, _ in runs} | {name for name, _ in left_out}
    for name in sorted(runners - names):
        print(f"run_host_tests_posix: run_{name}_test.bat is on no line of {manifest}")
    for name in sorted(names - runners):
        print(f"run_host_tests_posix: {manifest} names {name}, and tests/host has no run_{name}_test.bat")
    equal = runners == names
    print(f"posix manifest names: {len(names)} lines, {len(runners)} runners, {'equal' if equal else 'NOT equal'}")
    return 0 if equal else 1


# ---- which sources reach a Windows header or call a Microsoft-only name -------------------------------------------------

_DIRECTIVE = re.compile(r"\s*#\s*(ifdef|ifndef|if|elifdef|elifndef|elif|else|endif|include)\b(.*)")
_INCLUDE = re.compile(r'\s*(?:<([^>]+)>|"([^"]+)")')
_INCLUDE_START = re.compile(r"\s*#\s*include\s*")
_RAW_PREFIX = re.compile(r"(?<![A-Za-z0-9_])(?:u8|u|U|L)?R")
_RAW_OPEN = re.compile(r'"([^()\s]{0,16})\(')
_WORD_BEFORE = re.compile(r"[A-Za-z0-9_.']+$")
_ONLY_WIN = re.compile(r"defined\s*\(\s*_WIN32\s*\)|defined\s+_WIN32")
_NOT_WIN = re.compile(r"!\s*defined\s*\(\s*_WIN32\s*\)|!\s*defined\s+_WIN32")


def code_lines(text):
    """The lines of a C++ text with everything that is not code emptied: comments are dropped, a string or character
    literal becomes "" or '', a raw string literal R"x(...)x" becomes "" with its line breaks kept, so the line numbers
    stay. The name of an #include "..." line is kept. A directive or a call found in the result is real code."""
    out, cur, i, n = [], [], 0, len(text)

    def lines_of(skipped):
        for _ in range(skipped.count("\n")):
            out.append("".join(cur))
            cur.clear()

    def quoted_end(start, quote):
        """The index after the literal that opens at `start`: its closing quote, or the end of the line."""
        j = start + 1
        while j < n and text[j] != "\n":
            if text[j] == "\\":
                j += 2
                continue
            if text[j] == quote:
                return j + 1
            j += 1
        return min(j, n)

    while i < n:
        c = text[i]
        if c == "\n":
            lines_of(c)
            i += 1
        elif text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            cur.append(" ")
            lines_of(text[i:end])
            i = end
        elif c == '"':
            so_far = "".join(cur)
            raw = _RAW_OPEN.match(text, i) if (_RAW_PREFIX.search(so_far) and _RAW_PREFIX.search(so_far).end() == len(so_far)) else None
            if _INCLUDE_START.fullmatch(so_far):
                end = quoted_end(i, '"')
                cur.append(text[i:end])
            elif raw:
                close = ")" + raw.group(1) + '"'
                end = text.find(close, raw.end())
                end = n if end < 0 else end + len(close)
                cur.append('""')
                lines_of(text[i:end])
            else:
                end = quoted_end(i, '"')
                cur.append('""')
            i = end
        elif c == "'":
            tail = _WORD_BEFORE.search("".join(cur)[-40:])
            if tail and tail.group(0)[0].isdigit():  # a digit separator (1'000), not a character literal
                cur.append(c)
                i += 1
            else:
                i = quoted_end(i, "'")
                cur.append("''")
        else:
            cur.append(c)
            i += 1
    out.append("".join(cur))
    return out


def _scan(path):
    """What one file holds, in line order: [(line number, kind, name, in a Windows-only block)], kind 'angle' or 'quote'
    (an #include <...> or "...") or 'call' (a call spelled the Microsoft way, see MSVC_CALL).
    A block is Windows-only when it is the first branch of `#ifdef _WIN32` / `#if defined(_WIN32)`, the `#else` of
    `#ifndef _WIN32` / `#if !defined(_WIN32)`, or an `#elif defined(_WIN32)` / `#elifdef _WIN32` branch, and everything
    nested in one. Any other condition (`defined(_WIN32) || ...`, `_MSC_VER`, another #elif, #elifdef or #elifndef
    branch) is not: when in doubt a header or a call counts as reached."""
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    out, stack = [], []  # stack of states: 'win' (Windows only) / 'notwin' (the other branch of one) / 'other'
    for n, line in enumerate(code_lines(text), 1):
        windows_only = "win" in stack
        m = _DIRECTIVE.match(line)
        if not m:
            if not line.lstrip().startswith("#") or re.match(r"\s*#\s*define\b", line):
                out += [(n, "call", c.group(1), windows_only) for c in MSVC_CALL.finditer(line)
                        if c.group(1) not in PORTABLE_CALLS]
            continue
        word, rest = m.group(1), m.group(2).strip()
        if word == "ifdef":
            stack.append("win" if rest == "_WIN32" else "other")
        elif word == "ifndef":
            stack.append("notwin" if rest == "_WIN32" else "other")
        elif word == "if":
            stack.append("win" if _ONLY_WIN.fullmatch(rest) else "notwin" if _NOT_WIN.fullmatch(rest) else "other")
        elif word in ("elif", "elifdef", "elifndef") and stack:
            only_win = rest == "_WIN32" if word == "elifdef" else word == "elif" and _ONLY_WIN.fullmatch(rest) is not None
            stack[-1] = "win" if only_win else "other"
        elif word == "else" and stack:
            stack[-1] = {"win": "notwin", "notwin": "win"}.get(stack[-1], "other")
        elif word == "endif" and stack:
            stack.pop()
        elif word == "include":
            inc = _INCLUDE.match(rest)
            if inc:
                out.append((n, "angle" if inc.group(1) else "quote", (inc.group(1) or inc.group(2)).strip(), windows_only))
    return out


def _rel(path, repo):
    return os.path.relpath(path, repo).replace(os.sep, "/")


def _resolve(name, folder, repo, include_dirs):
    """The file a quoted include names: beside the including file, then from the repository root, then from src/, then
    from the run line's -I folders; None when it is in none of them."""
    for base in [folder, repo, os.path.join(repo, "src")] + [os.path.join(repo, d) for d in include_dirs]:
        path = os.path.normpath(os.path.join(base, name))
        if os.path.isfile(path):
            return path
    return None


def local_includes(path, repo, include_dirs=(), scanned=None):
    """(found, unresolved) for the quoted includes of one file that are outside a Windows-only block:
    found = [(line, name, the file it resolves to)], unresolved = [(line, name)] for a name that is in no folder."""
    found, unresolved = [], []
    for n, kind, name, windows_only in (_scan(path) if scanned is None else scanned):
        if kind == "quote" and not windows_only:
            target = _resolve(name, os.path.dirname(path), repo, include_dirs)
            if target is None:
                unresolved.append((n, name))
            else:
                found.append((n, name, target))
    return found, unresolved


def walk(source, repo, include_dirs=()):
    """What the compiler would see on a system that is not Windows, depth first in file order, each file once:
    yields (kind, file, line, name), kind 'angle' (a <...> include outside a Windows-only block), 'unresolved' (a
    quoted include that is in no folder: not followed, and reported here), 'guarded' (a <...> include inside a
    Windows-only block) or 'call' (a Microsoft-spelled call outside one). `file` is relative to `repo`, with '/'."""
    seen = set()

    def visit(path):
        key = os.path.normcase(os.path.abspath(path))
        if key in seen:
            return
        seen.add(key)
        scanned = _scan(path)
        found, unresolved = local_includes(path, repo, include_dirs, scanned)
        targets, missing = {n: target for n, _, target in found}, {n for n, _ in unresolved}
        for n, kind, name, windows_only in scanned:
            if kind == "angle":
                yield ("guarded" if windows_only else "angle", _rel(path, repo), n, name)
            elif kind == "call" and not windows_only:
                yield ("call", _rel(path, repo), n, name)
            elif kind == "quote" and n in missing:
                yield ("unresolved", _rel(path, repo), n, name)
            elif kind == "quote" and n in targets:
                yield from visit(targets[n])

    yield from visit(os.path.abspath(source))


def windows_reach(source, repo, include_dirs=()):
    """None, or (file, line, header) of the first Windows header `source` reaches outside a Windows-only block, through
    its local includes."""
    for kind, file, n, name in walk(source, repo, include_dirs):
        if kind == "angle" and name.replace("\\", "/").lower() in WINDOWS_HEADERS:
            return file, n, name
    return None


def unresolved_includes(source, repo, include_dirs=()):
    """[(file, line, name)] of the quoted includes in `source`'s closure that are in no folder (so were not followed)."""
    return [(file, n, name) for kind, file, n, name in walk(source, repo, include_dirs) if kind == "unresolved"]


def msvc_calls(source, repo, include_dirs=()):
    """[(file, line, name)] of the calls spelled the Microsoft way (MSVC_CALL, less PORTABLE_CALLS) that `source`'s
    closure makes outside a Windows-only block. A net over spellings: an empty list does not prove the code portable."""
    return [(file, n, name) for kind, file, n, name in walk(source, repo, include_dirs) if kind == "call"]


# ---- running ----------------------------------------------------------------------------------------------------------

def _command(argv):
    """`argv` as subprocess takes it. On Windows a .bat or .cmd is started through cmd.exe with the whole line in one
    pair of quotes, so that a space in its path or in an argument survives (the runner's tests use .bat stand-ins)."""
    if os.name == "nt" and argv[0].lower().endswith((".bat", ".cmd")):
        return 'cmd.exe /d /s /c "' + subprocess.list2cmdline(argv) + '"'
    return argv


def _run(argv, cwd, timeout=None):
    """(output, exit code) of a program run with no input, stdout and stderr together; exit code None when it was
    still running after `timeout` seconds (it and its child processes are then killed)."""
    kwargs = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {"start_new_session": True}
    try:
        p = subprocess.Popen(_command(argv), cwd=cwd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, **kwargs)
    except OSError as e:
        return f"could not start {argv[0]}: {e}\n", 127
    try:
        out, _ = p.communicate(timeout=timeout)
        return out.decode("utf-8", errors="replace"), p.returncode
    except subprocess.TimeoutExpired:
        if os.name == "nt":  # the whole tree: a child keeps the pipe open
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(p.pid)], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
        else:  # the program leads its own session (start_new_session), so its group is its tree
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except OSError:
                p.kill()
        try:
            out, _ = p.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            out = b""
        return out.decode("utf-8", errors="replace"), None


def run_test(name, sources, flags, cxx, repo, tmp, suffix, timeout, compile_timeout=DEFAULT_COMPILE_TIMEOUT):
    """(verdict, count, why, output, ran) for one run line."""
    if any("{SDK}" in f for f in flags):
        sdk = os.environ.get("REXSDK_DIR", "")
        if not sdk or not os.path.isdir(sdk):
            return "did not run", None, "REXSDK_DIR is not set or not a folder, and the line uses {SDK}", "", False
        flags = [f.replace("{SDK}", sdk) for f in flags]
    exe = os.path.join(tmp, name + suffix)
    output, code = _run([cxx, "-std=c++23", "-O2", *flags, *sources, "-o", exe], repo, compile_timeout)
    if code is None:
        return "did not run", None, f"compile timed out after {compile_timeout:g} s", output, False
    if code != 0 or not os.path.isfile(exe):  # a compiler that says 0 and wrote no program did not compile it
        return "did not run", None, f"compile failed; exit {code}", output, False
    output, code = _run([exe], os.path.join(repo, "tests", "host"), timeout)
    if code is None:  # a hung program never passes, whatever it printed before it hung
        return "did not run", None, f"timed out after {timeout:g} s", output, False
    return judge(name, output, code) + (output, True)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Compiles and runs the host tests of tests/host/posix_manifest.txt.")
    ap.add_argument("--only", nargs="+", metavar="NAME", help="run only these tests (run lines of the manifest)")
    ap.add_argument("--cxx", default="c++", help="the C++ compiler: one program name or path (default c++)")
    ap.add_argument("--manifest", help="the manifest (default tests/host/posix_manifest.txt of the repository)")
    ap.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="seconds each test program may take (default 600)")
    ap.add_argument("--compile-timeout", type=float, default=DEFAULT_COMPILE_TIMEOUT,
                    help="seconds each compile may take (default 1800); for this runner's own tests and slow machines")
    ap.add_argument("--repo", default=REPO, help="the repository root (this runner's own tests: a stand-in folder)")
    ap.add_argument("--exe-suffix", default=".exe" if os.name == "nt" else "",
                    help="the suffix of the program file the compiler is asked to write and that is then started "
                         "(default .exe on Windows, none elsewhere); it exists for this runner's own tests")
    ap.add_argument("--check-names", action="store_true",
                    help="compile nothing: compare the manifest's names with the run_<name>_test.bat files of tests/host")
    a = ap.parse_args(argv)
    if a.check_names and a.only:
        print("run_host_tests_posix: --check-names takes no --only")
        return 1
    repo = os.path.abspath(a.repo)
    manifest = a.manifest or os.path.join(repo, *MANIFEST.split("/"))
    try:
        with open(manifest, encoding="utf-8") as f:
            text = f.read()
    except OSError as e:
        print(f"run_host_tests_posix: cannot read the manifest {manifest}: {e}")
        return 1
    runs, left_out, errors = parse_manifest(text)
    if errors:
        for e in errors:
            print(f"run_host_tests_posix: {manifest}: {e}")
        return 1
    if a.check_names:
        return check_names(repo, manifest, runs, left_out)
    names = [name for name, _, _ in runs]
    if a.only:
        unknown = [n for n in a.only if n not in names]
        if unknown:
            print(f"run_host_tests_posix: no run line named {', '.join(unknown)} in {manifest}")
            return 1
    chosen = [r for r in runs if not a.only or r[0] in a.only]
    ran = passed = 0
    tmp = tempfile.mkdtemp(prefix="doa4_posix_host_")
    try:
        for name, sources, flags in chosen:
            verdict, count, why, output, did_run = run_test(name, sources, flags, a.cxx, repo, tmp, a.exe_suffix, a.timeout,
                                                            a.compile_timeout)
            ran += 1 if did_run else 0
            if verdict == "passed":
                passed += 1
                print(f"{name}: passed" + (f" {count}" if count is not None else ""), flush=True)
            else:
                print(f"{name}: {verdict} ({why})", flush=True)
                for line in output.splitlines()[-TAIL:]:
                    print(f"    | {line.rstrip()}")
                sys.stdout.flush()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    for name, reason in left_out:
        print(f"left out: {name} ({reason})")
    print(f"posix host tests: {ran} ran, {passed} passed, {len(left_out)} left out, manifest {len(runs)}")
    if a.only:
        return 0 if passed == len(chosen) else 1
    return 0 if runs and ran == len(runs) and passed == ran else 1


if __name__ == "__main__":
    sys.exit(main())
