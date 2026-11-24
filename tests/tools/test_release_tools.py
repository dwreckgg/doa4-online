"""Tests for the netplay release tools: tools/release_stage.py (the release's assets) and tools/run_host_tests.py (the host
tests as CI runs them). Synthetic folders only (no build, no game).

  py -3 -I tests/tools/test_release_tools.py
"""
import hashlib
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import traceback
import zipfile

sys.dont_write_bytecode = True  # load() must not leave tools/__pycache__ in the tree

if os.name == "nt":
    # The fake runners are batch files: give every process started here a console without a window.
    _Popen = subprocess.Popen

    class _QuietPopen(_Popen):
        def __init__(self, *args, **kwargs):
            kwargs["creationflags"] = kwargs.get("creationflags", 0) | subprocess.CREATE_NO_WINDOW
            super().__init__(*args, **kwargs)

    subprocess.Popen = _QuietPopen

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
TOOLS = os.path.join(REPO, "tools")
HOST = os.path.join(REPO, "tests", "host")
STAGE = os.path.join(TOOLS, "release_stage.py")
SECTION = os.path.join(TOOLS, "changelog_section.py")
RUN_HOST = os.path.join(TOOLS, "run_host_tests.py")
GAME_FILES = ["dead_or_alive_4.exe", "rexruntime.dll", "TracyClient.dll"]
PORT = "0123456789abcdef0123456789abcdef01234567"
SDK = "b643ea14b38f8475382840a5eb360b0d4090ae5e"
TAG = "alpha-v0.3.0"
ZIP = "DOA4-alpha-v0.3.0-win64.zip"
NOTICES = "THIRD_PARTY_NOTICES.md"
checks = 0
failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAIL", what)


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(TOOLS, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(script, *args, env=None):
    p = subprocess.run([sys.executable, "-I", script, *args], capture_output=True, text=True, encoding="utf-8",
                       errors="replace", env=env)
    return p.returncode, p.stdout + p.stderr


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def read(path):
    with open(path, "rb") as f:
        return f.read()


# Three different contents and sizes (one past 64 KiB, one empty), so a SHA256SUMS line that hashes the wrong file, or
# hashes one file for every name, gives a different line.
def contents():
    return {
        "dead_or_alive_4.exe": bytes((i * 7 + (i >> 8) * 13) % 256 for i in range(300_001)),
        "rexruntime.dll": b"rexruntime stand-in\n" * 50,
        "TracyClient.dll": b"",
    }


def make_build(root, skip=None, decoys=True):
    build = os.path.join(root, "build")
    for name, data in contents().items():
        if name != skip:
            write(os.path.join(build, name), data)
    if decoys:  # game data and other build outputs beside the three: the selection is by name, so none is staged
        for name in ("default.xex", "stg.afs", "intro.sfd", "x.dat", "dead_or_alive_4.pdb", "dead_or_alive_4.lib",
                     os.path.join("movie", "intro.sfd")):
            write(os.path.join(build, name), b"decoy " + name.encode())
    return build


# A synthetic CHANGELOG.md, newest first: the staged tag's version (0.3.0) sits between a newer and an older section,
# and each body has its own '###' headings, so a cut at the wrong line, or the wrong section, gives a different text.
BODY_030 = ("### New\n- Thirty feature one.\n- Thirty feature two.\n\n### Fixed\n- Thirty fix.\n\n"
            "### Notes\n- Thirty note.\n")
CHANGELOG = ("# Changelog\n\n"
             "## 0.3.1 (2026-11-11)\n\n### New\n- Thirty-one feature.\n\n"
             "## 0.3.0 (2026-11-08)\n\n" + BODY_030 + "\n"
             "## 0.2.9 (2026-11-01)\n\n### Fixed\n- Twenty-nine fix.\n")


def stage(root, build, *extra, launcher=True, out_name="staging", tag=TAG, changelog=CHANGELOG):
    """changelog: the text of a CHANGELOG.md passed with --changelog, or None to leave the option out (the repo's file)."""
    out = os.path.join(root, out_name)
    args = ["--build", build, "--out", out, "--tag", tag, "--port-commit", PORT, "--sdk-commit", SDK]
    if changelog is not None:
        cp = os.path.join(root, "CHANGELOG_" + out_name + ".md")
        write(cp, changelog.encode("utf-8"))
        args += ["--changelog", cp]
    if launcher:
        lz = os.path.join(root, "launcher_pkg.zip")
        write(lz, b"PK launcher stand-in")
        args += ["--launcher-zip", lz]
    code, text = run(STAGE, *args, *extra)
    return code, text, out


def staged_names(out):
    return sorted(os.listdir(out)) if os.path.isdir(out) else []


# ---- release_stage.py ---------------------------------------------------------------------------------------------

def test_stage_exact_set(root):
    build = make_build(root)
    code, text, out = stage(root, build)
    check(code == 0, f"stage: exit {code}\n{text}")
    want = sorted(GAME_FILES + ["SHA256SUMS.txt", NOTICES, ZIP, "DOA4-launcher.zip"])
    check(staged_names(out) == want, f"stage: the staging set is exactly {want}: {staged_names(out)}")
    for name, data in contents().items():
        p = os.path.join(out, name)
        check(os.path.isfile(p) and read(p) == data, f"stage: {name} is a byte copy of the build's")
    # SHA256SUMS: one line per game file, '<lower hex>  <name>', in the order exe, rexruntime, Tracy; the oracle is hashlib
    want_sums = "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in contents().items())
    sums = read(os.path.join(out, "SHA256SUMS.txt")) if os.path.isfile(os.path.join(out, "SHA256SUMS.txt")) else b""
    check(sums == want_sums.encode("ascii"), f"stage: SHA256SUMS.txt is hashlib's three lines\n{sums!r}\nwant\n{want_sums!r}")
    check(sums.count(b"\n") == 3 and b"\r" not in sums, f"stage: SHA256SUMS.txt has exactly three LF lines (no line for the zips): {sums!r}")
    # the zip: the three, SHA256SUMS.txt and README-netplay.txt, nothing else; each member equal to the staged file
    zp = os.path.join(out, ZIP)
    members = {}
    if os.path.isfile(zp):
        with zipfile.ZipFile(zp) as z:
            members = {i.filename: z.read(i) for i in z.infolist()}
    want_members = sorted(GAME_FILES + ["SHA256SUMS.txt", NOTICES, "README-netplay.txt"])
    check(sorted(members) == want_members, f"stage: zip members exactly {want_members}: {sorted(members)}")
    for name in GAME_FILES + ["SHA256SUMS.txt"]:
        p = os.path.join(out, name)
        check(name in members and os.path.isfile(p) and members[name] == read(p), f"stage: zip member {name} equals the staged file")
    # R4-2: the BSD notice duty: THIRD_PARTY_NOTICES.md (the repository's own file) is staged and is in the zip, byte for byte
    notices = read(os.path.join(REPO, NOTICES))
    check(len(notices) > 100, f"stage: the repository's {NOTICES} is a real file ({len(notices)} bytes)")
    check(os.path.isfile(os.path.join(out, NOTICES)) and read(os.path.join(out, NOTICES)) == notices, f"stage: staged {NOTICES} is the repository's file")
    check(members.get(NOTICES) == notices, f"stage: zip member {NOTICES} is the repository's file")
    # the notices of the ported code (P3D-12): numpy (BSD-3), CPython (PSF-2.0) and the libm of Plan 2 (musl), each by name
    for needle in ("numpy", "Python Software Foundation", "musl"):
        check(needle in notices.decode("utf-8", "replace"), f"stage: {NOTICES} names {needle!r}")
    readme = members.get("README-netplay.txt", b"").decode("utf-8", "replace")
    # R4-1: the firewall advice (both players, Private AND Public, Alt+Tab, UPnP or forward UDP 7200) and the R1-2 line
    for needle in ("first time you host or join", "both players", "Private", "Public", "Alt+Tab", "UPnP", "UDP 7200"):
        check(needle in readme, f"stage: README-netplay.txt says {needle!r}\n{readme}")
    for needle in (TAG, PORT, SDK, "SHA256SUMS.txt", "no game data"):
        check(needle.lower() in readme.lower(), f"stage: README-netplay.txt names {needle!r}\n{readme}")
    # P3D-6 / Task 7 step 5: the build's public name first, and the optional swimsuit line (no DOAX2 data in the release)
    check(readme.startswith(f"Dead or Alive 4 Online {TAG}"), f"stage: README-netplay.txt starts with the build's name and tag: {readme[:60]!r}")
    check("USA copy of Dead or Alive Xtreme 2" in readme and "No DOAX2 data is in this release." in readme,
          f"stage: README-netplay.txt has the optional swimsuit line: {readme}")
    # Task 13: the tag's CHANGELOG section (0.3.0's whole body, CRLF like the rest of the file), under its own line;
    # NOT the newer (0.3.1) or the older (0.2.9) section, and not a '## ' heading line
    want_changes = ("What's new in 0.3.0:\r\n\r\n" + BODY_030.replace("\n", "\r\n")).encode("ascii")
    raw = members.get("README-netplay.txt", b"")
    check(want_changes in raw, f"stage: README-netplay.txt carries 0.3.0's CHANGELOG section\n{raw!r}")
    for needle in ("Thirty-one", "Twenty-nine", "## 0."):
        check(needle not in readme, f"stage: README-netplay.txt has no {needle!r} (another version's section)\n{readme}")
    check(b"\r\n" in raw and b"\n" not in raw.replace(b"\r\n", b""), f"stage: README-netplay.txt has CRLF line ends only\n{raw!r}")
    lp =os.path.join(out, "DOA4-launcher.zip")
    check(os.path.isfile(lp) and read(lp) == b"PK launcher stand-in", "stage: DOA4-launcher.zip is a copy of --launcher-zip")


def test_stage_without_launcher(root):
    build = make_build(root, decoys=False)
    code, text, out = stage(root, build, launcher=False)
    want = sorted(GAME_FILES + ["SHA256SUMS.txt", NOTICES, ZIP])
    check(code == 0 and staged_names(out) == want, f"stage without --launcher-zip: exit {code}, {staged_names(out)}\n{text}")


def test_stage_missing_file(root):
    for name in GAME_FILES:
        sub = os.path.join(root, "m_" + name)
        build = make_build(sub, skip=name)
        code, text, out = stage(sub, build)
        check(code == 1, f"stage, {name} missing: exit {code}\n{text}")
        check(name in text and "missing" in text, f"stage, {name} missing: the message names it\n{text}")
        check(staged_names(out) == [], f"stage, {name} missing: nothing staged: {staged_names(out)}")


def test_stage_game_data_refused_by_pattern(root):
    # the pattern rule is checked before the list rule, so its own message shows (a mutant without it still exits 1,
    # through the list rule, and is caught by the message check)
    build = make_build(root)
    for i, extra in enumerate(("default.xex", "stg.afs", "STG.AFS", "intro.sfd", "x.dat")):
        if not os.path.exists(os.path.join(build, extra)):
            write(os.path.join(build, extra), b"decoy")
        code, text, out = stage(root, build, "--extra", extra, out_name=f"out_p{i}")
        check(code == 1 and extra in text and "game data" in text, f"stage --extra {extra}: refused as game data: exit {code}\n{text}")
        check(staged_names(out) == [], f"stage --extra {extra}: nothing staged: {staged_names(out)}")
    for i, extra in enumerate(("movie/intro.sfd", "movie\\notes.txt")):
        write(os.path.join(build, *re.split(r"[\\/]", extra)), b"decoy")
        code, text, out = stage(root, build, "--extra", extra, out_name=f"out_s{i}")
        check(code == 1 and extra in text and "under a folder" in text, f"stage --extra {extra}: refused (under a folder): exit {code}\n{text}")
        check(staged_names(out) == [], f"stage --extra {extra}: nothing staged: {staged_names(out)}")


def test_stage_extra_file_refused(root):
    build = make_build(root)
    write(os.path.join(build, "notes.txt"), b"not a release file")
    code, text, out = stage(root, build, "--extra", "notes.txt")
    check(code == 1 and "notes.txt" in text and "not one of the release files" in text,
          f"stage --extra notes.txt: refused by the list rule: exit {code}\n{text}")
    check(staged_names(out) == [], f"stage --extra notes.txt: nothing staged: {staged_names(out)}")
    code, text, out = stage(root, build, "--extra", "rexruntime.dll", out_name="out_dup")
    check(code == 1 and "rexruntime.dll" in text and "twice" in text, f"stage --extra rexruntime.dll: refused (named twice): exit {code}\n{text}")
    check(staged_names(out) == [], f"stage --extra rexruntime.dll: nothing staged: {staged_names(out)}")


def test_stage_refuses_a_used_folder_and_a_bad_tag(root):
    build = make_build(root, decoys=False)
    out = os.path.join(root, "staging")
    write(os.path.join(out, "old.txt"), b"left from an earlier run")
    code, text, out = stage(root, build)
    check(code == 1 and "not empty" in text, f"stage into a non-empty folder: refused: exit {code}\n{text}")
    check(staged_names(out) == ["old.txt"], f"stage into a non-empty folder: nothing added: {staged_names(out)}")
    for i, tag in enumerate(("netplay-v1.0", "netplay-v1.0.7/../x", "")):
        o = os.path.join(root, f"out_t{i}")
        code, text = run(STAGE, "--build", build, "--out", o, "--tag", tag, "--port-commit", PORT, "--sdk-commit", SDK)
        check(code == 1 and "--tag" in text, f"stage --tag {tag!r}: refused by the tag rule: exit {code}\n{text}")
        check(staged_names(o) == [], f"stage --tag {tag!r}: nothing staged: {staged_names(o)}")


def test_stage_changelog_refusals(root):
    # Task 13: no section for the tag's version, or no CHANGELOG file: refused with the clean message, nothing staged
    build = make_build(root, decoys=False)
    no_030 = CHANGELOG.replace("## 0.3.0 (2026-11-08)", "## 0.3.00 (2026-11-08)")  # a near miss, not 0.3.0's heading
    code, text, out = stage(root, build, changelog=no_030, out_name="out_nosec")
    check(code == 1 and "0.3.0" in text and "no section" in text and "Traceback" not in text,
          f"stage, no 0.3.0 section: refused with the message: exit {code}\n{text}")
    check(staged_names(out) == [], f"stage, no 0.3.0 section: nothing staged: {staged_names(out)}")
    missing = os.path.join(root, "no such CHANGELOG.md")
    code, text, out = stage(root, build, "--changelog", missing, changelog=None, out_name="out_nofile")
    check(code == 1 and "not found" in text and "Traceback" not in text,
          f"stage, no CHANGELOG file: refused with the message: exit {code}\n{text}")
    check(staged_names(out) == [], f"stage, no CHANGELOG file: nothing staged: {staged_names(out)}")


def test_stage_default_changelog(root):
    # called as the workflow calls it (no --changelog): the repository's CHANGELOG.md, here its 0.3.1 section
    build = make_build(root, decoys=False)
    code, text, out = stage(root, build, launcher=False, tag="alpha-v0.3.1", changelog=None)
    check(code == 0, f"stage alpha-v0.3.1 with the repository's CHANGELOG.md: exit {code}\n{text}")
    zp = os.path.join(out, "DOA4-alpha-v0.3.1-win64.zip")
    raw = b""
    if os.path.isfile(zp):
        with zipfile.ZipFile(zp) as z:
            raw = z.read("README-netplay.txt") if "README-netplay.txt" in z.namelist() else b""
    for needle in (b"What's new in 0.3.1:\r\n\r\n### New\r\n- Language and subtitles are saved",
                   b"### Notes\r\n- The first start after updating rebuilds the shader cache, so it is slower once.\r\n"):
        check(needle in raw, f"stage alpha-v0.3.1: README-netplay.txt carries {needle!r}\n{raw!r}")


def refusal(fn, *args):
    """The message of the NoSection that fn(*args) raises, or None when it returns."""
    try:
        fn(*args)
    except Exception as e:  # any other class is reported by the caller's check on the class name
        return f"{type(e).__name__}: {e}"
    return None


def section_or_refusal(cs, text, version):
    """cs.section(text, version), or the refusal's text: a wrong refusal fails its own check, not the whole test."""
    try:
        return cs.section(text, version)
    except Exception as e:
        return f"{type(e).__name__}: {e}"


def test_changelog_section(root):
    cs = load("changelog_section")
    sec = lambda text, version: section_or_refusal(cs, text, version)  # noqa: E731
    # the body under '## <version> (...)' up to the next '## ' heading: the middle, the first (newest) and the last
    check(sec(CHANGELOG, "0.3.0") == BODY_030, f"section 0.3.0: exactly its body: {sec(CHANGELOG, '0.3.0')!r}")
    check(sec(CHANGELOG, "0.3.1") == "### New\n- Thirty-one feature.\n", f"section 0.3.1 (the first): its body, not 0.3.0's: {sec(CHANGELOG, '0.3.1')!r}")
    check(sec(CHANGELOG, "0.2.9") == "### Fixed\n- Twenty-nine fix.\n", f"section 0.2.9 (the last): its body to the end: {sec(CHANGELOG, '0.2.9')!r}")
    check(sec(CHANGELOG.rstrip("\n"), "0.2.9") == "### Fixed\n- Twenty-nine fix.\n",
          "section 0.2.9, no newline at the end of the file: the same body")
    # a CRLF checkout (git's autocrlf on a Windows runner) gives the same LF body
    check(sec(CHANGELOG.replace("\n", "\r\n"), "0.3.0") == BODY_030, "section 0.3.0 of a CRLF file: the same LF body")
    # refused: no section, a heading that only starts with the version, an empty section, the version twice
    for text, version, token in ((CHANGELOG, "0.4.0", "no section"),
                                 ("## 0.3.10 (2026-12-01)\n\n- Ten.\n", "0.3.1", "no section"),
                                 ("## 0.3.0 (2026-11-08)\n\n\n## 0.2.9 (2026-11-01)\n- Fix.\n", "0.3.0", "empty"),
                                 (CHANGELOG + "\n## 0.3.0 (2026-10-01)\n- Again.\n", "0.3.0", "twice")):
        msg = refusal(cs.section, text, version)
        check(msg is not None and msg.startswith("NoSection") and token in msg and version in msg,
              f"section {version} of {text[:40]!r}...: NoSection naming it ({token!r}): {msg!r}")
    # the command line, as the workflow calls it: --out gets the body as LF bytes, no BOM; without --out, stdout
    cp = os.path.join(root, "CHANGELOG.md")
    write(cp, CHANGELOG.replace("\n", "\r\n").encode("ascii"))
    o = os.path.join(root, "changes.md")
    code, text = run(SECTION, "0.3.0", "--changelog", cp, "--out", o)
    check(code == 0 and os.path.isfile(o) and read(o) == BODY_030.encode("ascii"),
          f"changelog_section 0.3.0 --out: exit {code}, the body's bytes: {read(o) if os.path.isfile(o) else None!r}\n{text}")
    code, text = run(SECTION, "0.3.0", "--changelog", cp)
    check(code == 0 and text == BODY_030, f"changelog_section 0.3.0 to stdout: exit {code}: {text!r}")
    for i, (args, token) in enumerate(((["0.4.0", "--changelog", cp], "no section"),
                                       (["0.4", "--changelog", cp], "x.y.z"),
                                       (["0.4.0", "--changelog", os.path.join(root, "absent.md")], "not found"))):
        o = os.path.join(root, f"refused_{i}.md")
        code, text = run(SECTION, *args, "--out", o)
        check(code == 1 and token in text and args[0] in text and "Traceback" not in text,
              f"changelog_section {args[0]} ({token}): exit 1 with the message: exit {code}\n{text}")
        check(not os.path.exists(o), f"changelog_section {args[0]} ({token}): no --out file written")


def test_repo_changelog(root):
    # the repository's CHANGELOG.md: ASCII, newest first, and the 0.3.1 section has the three headings. A checkout with
    # git's autocrlf (a Windows runner) has CRLF, so the line ends are read as the checkout has them: all CRLF or all LF.
    cs = load("changelog_section")
    raw = read(os.path.join(REPO, "CHANGELOG.md"))
    if raw.count(b"\r\n") == raw.count(b"\n"):
        raw = raw.replace(b"\r\n", b"\n")
    check(raw.isascii() and b"\r" not in raw and raw.startswith(b"# Changelog\n\n## "),
          f"CHANGELOG.md: ASCII, one kind of line end, '# Changelog' then a '## ' section: {raw[:40]!r}")
    versions = [tuple(int(n) for n in m) for m in re.findall(rb"^## (\d+)\.(\d+)\.(\d+) \(", raw, re.M)]
    check(len(versions) >= 1 and versions == sorted(versions, reverse=True), f"CHANGELOG.md: sections newest first: {versions}")
    body = section_or_refusal(cs, raw.decode("ascii", "replace"), "0.3.1")
    check(body.startswith("### New\n- ") and "\n### Fixed\n- " in body and "\n### Notes\n- " in body,
          f"CHANGELOG.md 0.3.1: New, Fixed, Notes: {body!r}")
    # the version the release workflow will publish (DOA4_NETPLAY_VERSION, read as its Version step reads it) has a
    # section, so a raised version without notes fails here, not at the end of the release job
    cmake = read(os.path.join(REPO, "CMakeLists.txt")).decode("utf-8", "replace")
    m = re.search(r'set\(DOA4_NETPLAY_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"', cmake)
    version = m.group(1) if m else "?"
    got = section_or_refusal(cs, raw.decode("ascii", "replace"), version)
    check(m is not None and not got.startswith("NoSection"), f"CHANGELOG.md has a section for DOA4_NETPLAY_VERSION {version}: {got!r}")


# ---- run_host_tests.py --------------------------------------------------------------------------------------------

def write_bat(folder, name, lines):
    write(os.path.join(folder, f"run_{name}_test.bat"), ("\r\n".join(["@echo off"] + lines) + "\r\n").encode("ascii"))


def host_run(*args):
    return run(RUN_HOST, *args)


def line_for(text, name):
    for line in text.splitlines():
        if line.startswith(name + ": "):
            return line
    return None


def test_runner_verdicts(root):
    d = os.path.join(root, "host tests")  # a space in the path, as a user's checkout may have
    write_bat(d, "a", ["echo PASSED: 3 checks, 0 failures", "exit /b 0"])
    write_bat(d, "b", ["echo FAILED: 3 checks, 1 failures", "exit /b 1"])
    write_bat(d, "c", ["exit /b 0"])
    code, text = host_run("--dir", d)
    check(code == 1, f"run_host_tests a/b/c: exit {code}\n{text}")
    check(line_for(text, "a") == "a: passed 3", f"run_host_tests: 'a: passed 3': {line_for(text, 'a')!r}\n{text}")
    check((line_for(text, "b") or "").startswith("b: FAILED"), f"run_host_tests: 'b: FAILED': {line_for(text, 'b')!r}\n{text}")
    check((line_for(text, "c") or "").startswith("c: did not run (no summary line"), f"run_host_tests: c did not run: {line_for(text, 'c')!r}\n{text}")
    check("FAILED: 3 checks, 1 failures" in text.split(line_for(text, "b") or "\0", 1)[-1],
          f"run_host_tests: the failed runner's output follows its line\n{text}")
    # --only: just the named runner
    code, text = host_run("--dir", d, "--only", "a")
    check(code == 0 and line_for(text, "a") == "a: passed 3" and line_for(text, "b") is None and line_for(text, "c") is None,
          f"run_host_tests --only a: exit {code}\n{text}")
    # --only with a name no runner has: refused, never a vacuous pass
    code, text = host_run("--dir", d, "--only", "zz")
    check(code != 0 and "zz" in text, f"run_host_tests --only zz: refused: exit {code}\n{text}")
    # an empty folder: no runner found is not a pass
    e = os.path.join(root, "empty")
    os.makedirs(e)
    code, text = host_run("--dir", e)
    check(code != 0 and "no runner" in text, f"run_host_tests on an empty folder: refused: exit {code}\n{text}")


def test_runner_exit_code_and_vacuous_pass(root):
    d = os.path.join(root, "host")
    write_bat(d, "x7", ["echo PASSED: 3 checks, 0 failures", "exit /b 7"])  # the exit code reaches the judge
    write_bat(d, "f0", ["echo FAILED: 3 checks, 1 failures", "exit /b 0"])  # a FAILED summary is never a pass
    write_bat(d, "z", ["echo PASSED: 0 checks, 0 failures", "exit /b 0"])   # no check ran: not a pass
    write_bat(d, "ok", ["echo FAILED: 1 checks, 1 failures", "echo PASSED: 12 checks, 0 failures", "exit /b 0"])  # the last summary counts, not the first
    code, text = host_run("--dir", d)
    check(code == 1, f"run_host_tests exit codes: exit {code}\n{text}")
    lx = line_for(text, "x7") or ""
    check(lx.startswith("x7: FAILED") and "exit 7" in lx, f"run_host_tests: PASSED line but exit 7 -> FAILED with the code: {lx!r}\n{text}")
    check((line_for(text, "f0") or "").startswith("f0: FAILED"), f"run_host_tests: FAILED line with exit 0 -> FAILED\n{text}")
    check((line_for(text, "z") or "").startswith("z: FAILED"), f"run_host_tests: 0 checks -> FAILED\n{text}")
    check(line_for(text, "ok") == "ok: passed 12", f"run_host_tests: the last summary line counts: {line_for(text, 'ok')!r}\n{text}")


def test_runner_older_shapes(root):
    # runners whose tests print an older summary line (the table in run_host_tests.py), under their real names
    d = os.path.join(root, "host")
    write_bat(d, "toml_upsert", ["echo toml_upsert: all checks passed", "exit /b 0"])
    write_bat(d, "backup", ["echo 2 check(s) failed", "exit /b 1"])
    write_bat(d, "speakers", ["echo all speakers checks passed", "exit /b 0"])
    write_bat(d, "dpps", ["echo canonical NaN results all 0xFFC00000 and the redirect agrees with the SDK function: yes", "exit /b 0"])
    code, text = host_run("--dir", d)
    check(code == 1, f"run_host_tests older shapes: exit {code} (backup failed)\n{text}")
    check(line_for(text, "toml_upsert") == "toml_upsert: passed", f"older shape: toml_upsert passed\n{text}")
    check((line_for(text, "backup") or "").startswith("backup: FAILED"), f"older shape: backup FAILED\n{text}")
    check(line_for(text, "speakers") == "speakers: passed", f"older shape: speakers passed\n{text}")
    check(line_for(text, "dpps") == "dpps: passed", f"older shape: dpps passed\n{text}")
    write_bat(d, "dpps", ["echo canonical NaN results all 0xFFC00000 and the redirect agrees with the SDK function: NO", "exit /b 1"])
    write_bat(d, "speakers", ["echo 1 FAILED", "exit /b 1"])
    write_bat(d, "toml_upsert", ["echo PASSED: 4 checks, 0 failures", "exit /b 0"])  # not toml_upsert's own line
    code, text = host_run("--dir", d, "--only", "dpps", "speakers", "toml_upsert")
    check(code == 1, f"run_host_tests older shapes, failing: exit {code}\n{text}")
    check((line_for(text, "dpps") or "").startswith("dpps: FAILED"), f"older shape: dpps NO -> FAILED\n{text}")
    check((line_for(text, "speakers") or "").startswith("speakers: FAILED"), f"older shape: speakers FAILED\n{text}")
    check((line_for(text, "toml_upsert") or "").startswith("toml_upsert: did not run"),
          f"older shape: a line that is not the runner's own is no summary\n{text}")


def test_runner_timeout(root):
    # R4-5: a runner that hangs is "did not run (timed out ...)", fails the run, and does not stop the others
    d = os.path.join(root, "host")
    write_bat(d, "hang", ["echo PASSED: 3 checks, 0 failures", "ping -n 60 127.0.0.1 >nul", "exit /b 0"])  # a summary line, then a sleep
    write_bat(d, "zok", ["echo PASSED: 3 checks, 0 failures", "exit /b 0"])
    t0 = time.time()
    code, text = host_run("--dir", d, "--timeout", "3")
    took = time.time() - t0
    check(code == 1, f"run_host_tests timeout: exit {code}\n{text}")
    check((line_for(text, "hang") or "").startswith("hang: did not run (timed out"), f"run_host_tests timeout: 'hang: did not run (timed out': {line_for(text, 'hang')!r}\n{text}")
    check(line_for(text, "zok") == "zok: passed 3", f"run_host_tests timeout: the next runner still runs: {line_for(text, 'zok')!r}\n{text}")
    check("1 passed, 0 failed, 1 did not run" in text, f"run_host_tests timeout: the count line\n{text}")
    check(took < 40, f"run_host_tests timeout: the hung runner was stopped after the limit, not after its sleep ({took:.0f} s)")
    # the default is 600 s, and a runner within its limit is judged as before
    check(load("run_host_tests").DEFAULT_TIMEOUT == 600, "run_host_tests: the default per-runner timeout is 600 s")
    code, text = host_run("--dir", d, "--only", "zok", "--timeout", "30")
    check(code == 0 and line_for(text, "zok") == "zok: passed 3", f"run_host_tests, within the limit: exit {code}\n{text}")


def shape_found(source, alternatives):
    """The first alternative (a list of literals) of which every literal is in `source`, or None."""
    for alt in alternatives:
        if all(w in source for w in alt):
            return alt
    return None


def test_shape_found(root):
    # the "default" shape is printed in one of two source forms; each form needs BOTH of its literals
    two = [['"%s: %d checks, %d failures', '"FAILED" : "PASSED"'], ['"FAILED: %d checks, %d failures', '"PASSED: %d checks, 0 failures']]
    ternary = 'printf("%s: %d checks, %d failures\n", f ? "FAILED" : "PASSED", a, b);'
    split = 'printf("FAILED: %d checks, %d failures\n", a, b); printf("PASSED: %d checks, 0 failures\n", a);'
    check(shape_found(ternary, two) == two[0] and shape_found(split, two) == two[1], "shape_found: both real source forms are accepted")
    check(shape_found('printf("PASSED: %d checks, 0 failures\n", a);', two) is None, "shape_found: a source with only the PASSED line is refused")
    check(shape_found('printf("FAILED: %d checks, %d failures\n", a, b);', two) is None, "shape_found: a source with only the FAILED line is refused")
    check(shape_found('printf("%s: %d checks, %d failures\n", a, b, c);', two) is None, "shape_found: a source that never says FAILED / PASSED is refused")
    check(shape_found("", two) is None, "shape_found: an empty source is refused")


def test_table_matches_sources(root):
    # every real runner: the summary shape run_host_tests.py expects is the one its test source prints
    rht = load("run_host_tests")
    literals = {
        "default": [['"%s: %d checks, %d failures', '"FAILED" : "PASSED"'],
                    ['"FAILED: %d checks, %d failures', '"PASSED: %d checks, 0 failures']],
        "speakers": [['"all speakers checks passed', '"%d FAILED']],
        "dpps": [['the redirect agrees with the SDK function: %s', '"yes" : "NO"']],
    }
    bats = sorted(f for f in os.listdir(HOST) if re.fullmatch(r"run_\w+_test\.bat", f))
    check(len(bats) >= 30, f"table: the real runners are found: {len(bats)}")
    for bat in bats:
        name = bat[len("run_"):-len("_test.bat")]
        text = read(os.path.join(HOST, bat)).decode("utf-8", "replace")
        exe = re.findall(r'/Fe"%OUT%\\(\w+)\.exe"', text)
        src = os.path.join(HOST, exe[0] + ".cpp") if exe else ""
        source = read(src).decode("utf-8", "replace") if src and os.path.isfile(src) else ""
        kind = rht.shape_kind(name)
        want = literals.get(kind) or [[f'"{name}: all checks passed', '"%d check(s) failed']]
        check(bool(source) and shape_found(source, want) is not None,
              f"table: {bat} ({kind}) prints one of {want} in {os.path.basename(src) or '?'}")


def test_runners_use_the_pinned_llvm(root):
    # every real runner compiles with the LLVM the release workflow pins (DOA4_LLVM_BIN, set to C:\LLVM\bin there): a
    # runner that puts a literal LLVM folder first on PATH compiles with the image's own LLVM in CI, which a newer Visual
    # Studio STL refuses (0.3.7's first release run: run_afs_overlay_test.bat, clang 20 against STL 14.52)
    bats = sorted(f for f in os.listdir(HOST) if re.fullmatch(r"run_\w+_test\.bat", f))
    check(len(bats) >= 30, f"llvm: the real runners are found: {len(bats)}")
    for bat in bats:
        text = read(os.path.join(HOST, bat)).decode("utf-8", "replace")
        lines = [l.strip() for l in text.splitlines()]
        default = any(l.lower().startswith('if not defined doa4_llvm_bin set "doa4_llvm_bin=') for l in lines)
        uses = any(l.startswith('set "PATH=%DOA4_LLVM_BIN%;') for l in lines)
        literal = [l for l in lines if re.match(r'set "PATH=[A-Za-z]:', l)]
        check(default and uses and not literal,
              f"llvm: {bat} defaults DOA4_LLVM_BIN, puts it first on PATH and no literal folder before it"
              + ("" if default and uses and not literal else f" (default {default}, uses {uses}, literal {literal})"))


# ---- PGO: pgo_source_id.py, pgo_recipe.py, the workflow and np_build.bat wiring ------------------------------------

PGO_ID = os.path.join(TOOLS, "pgo_source_id.py")
PGO_RECIPE = os.path.join(TOOLS, "pgo_recipe.py")
WORKFLOW = os.path.join(REPO, ".github", "workflows", "netplay-release.yml")
NP_BUILD = os.path.join(TOOLS, "np_build.bat")
SDK_A = "4fe33826f8387d0e98e64bbcb5923355c45d9898"
SDK_B = "29495ab15083d8aa77bd7b560bdd39689ebea077"
# G3's list, written out here (not read from the tool), so a tool that drops or adds a path fails source_id_moves
ID_FILES = {"src/x.cpp": b"int x = 1;\n", "src/net/y.cpp": b"int y = 2;\n", "generated/g.cpp": b"int g = 3;\n",
            "third_party/t/t.h": b"#define T 4\n", "CMakeLists.txt": b"project(doa4)\n",
            "CMakePresets.json": b"{\"version\": 6}\n", "hints.toml": b"[hints]\na = 1\n",
            "dead_or_alive_4_manifest.toml": b"[manifest]\nb = 2\n"}
OTHER_FILES = {"tools/y.py": b"print(1)\n", "README.md": b"# readme\n",
               "netplay_sdk.txt": f"repo=https://example.invalid/sdk\ncommit={SDK_A}\n".encode("ascii")}
PROFILE_BYTES = b"\xff\x00synthetic profile, not a real one\x00\r\n\x01"  # never a real profile


def git(repo, *args):
    p = subprocess.run(["git", "-C", repo, "-c", "user.name=t", "-c", "user.email=t@t", *args], capture_output=True,
                       text=True, encoding="utf-8", errors="replace", timeout=120)
    return p.returncode, p.stdout + p.stderr


def make_repo(root, name="game", files=None):
    repo = os.path.join(root, name)
    for rel, data in (files or {**ID_FILES, **OTHER_FILES}).items():
        write(os.path.join(repo, *rel.split("/")), data)
    git(repo, "init", "-q")
    commit(repo, "init")
    return repo


def commit(repo, msg="change"):
    git(repo, "add", "-A")
    code, text = git(repo, "commit", "-q", "-m", msg)
    check(code == 0, f"fixture: git commit '{msg}' in {repo}: exit {code}\n{text}")


def head(repo):
    code, text = git(repo, "rev-parse", "HEAD")
    return text.strip() if code == 0 else "?"


def pgo(*args):
    return run(PGO_ID, *args)


def compute(repo, sdk=SDK_A):
    return pgo("compute", "--repo", repo, *(["--sdk", sdk] if sdk else []))


def line_of(text, prefix):
    for line in text.splitlines():
        if line.startswith(prefix):
            return line
    return None


def put_pgo(repo, source=None, profile=True):
    """pgo/doa4.profdata (synthetic bytes) and pgo/source.txt (the given text), each only when asked."""
    if profile:
        write(os.path.join(repo, "pgo", "doa4.profdata"), PROFILE_BYTES)
    if source is not None:
        write(os.path.join(repo, "pgo", "source.txt"), source.encode("ascii"))


def use_line(repo):
    return "pgo: use " + os.path.abspath(os.path.join(repo, "pgo", "doa4.profdata")).replace("\\", "/")


def oracle_game(repo):
    """G3 recomputed here: sha256 of the sorted 'git ls-tree -r HEAD' lines of the seven paths, one LF after each."""
    paths = ["src", "generated", "third_party", "CMakeLists.txt", "CMakePresets.json", "hints.toml",
             "dead_or_alive_4_manifest.toml"]
    p = subprocess.run(["git", "ls-tree", "-r", "HEAD", "--", *paths], cwd=repo, capture_output=True)
    lines = sorted(p.stdout.decode("utf-8").splitlines())
    return hashlib.sha256("".join(l + "\n" for l in lines).encode("utf-8")).hexdigest(), len(lines)


def test_pgo_source_id_stable(root):
    repo = make_repo(root)
    c1, t1 = compute(repo)
    c2, t2 = compute(repo)
    check(c1 == 0 and c2 == 0 and t1 == t2, f"source id: same tree twice, same output: exit {c1}/{c2}\n{t1}\n{t2}")
    want, n = oracle_game(repo)
    check(n == len(ID_FILES), f"source id oracle: the fixture has {len(ID_FILES)} tracked files under the seven paths: {n}")
    check(t1 == f"game={want}\nsdk={SDK_A}\nllvm=23.1.4\nnames=portable\n", f"source id: exactly the four lines (G3 + names):\n{t1!r}\nwant game={want}")
    # without --sdk, the sdk line is the pin's commit
    c3, t3 = compute(repo, sdk=None)
    check(c3 == 0 and line_of(t3, "sdk=") == f"sdk={SDK_A}", f"source id without --sdk: the pin's commit: exit {c3}\n{t3}")
    # a change under pgo/, in tools/, the README and the SDK pin, committed: the game line is the same
    put_pgo(repo, source=t1)
    write(os.path.join(repo, "tools", "y.py"), b"print(2)\n")
    write(os.path.join(repo, "README.md"), b"# readme, changed\n")
    write(os.path.join(repo, "netplay_sdk.txt"), f"repo=https://example.invalid/sdk\ncommit={SDK_B}\n".encode("ascii"))
    commit(repo, "outside the id")
    c4, t4 = compute(repo)
    check(c4 == 0 and line_of(t4, "game=") == line_of(t1, "game=") and line_of(t1, "game=") is not None,
          f"source id: pgo/, tools/, README and netplay_sdk.txt do not move the game line: exit {c4}\n{t1}\n{t4}")
    # a short or non-hex --sdk is refused (the check compares full commit ids as text)
    for bad in ("4fe33826", "z" * 40):
        c5, t5 = compute(repo, sdk=bad)
        check(c5 == 1 and "pgo: refused:" in t5 and "Traceback" not in t5, f"compute --sdk {bad}: refused: exit {c5}\n{t5}")


def test_pgo_source_id_moves(root):
    repo = make_repo(root)
    code, text = compute(repo)
    before = line_of(text, "game=")
    check(code == 0 and before is not None, f"source id moves: first compute: exit {code}\n{text}")
    # one byte in a file under each of the seven paths, committed: each moves the game line (a new value each time)
    seen = {before}
    for rel, data in ID_FILES.items():
        if rel == "src/net/y.cpp":
            continue
        write(os.path.join(repo, *rel.split("/")), data.replace(b"\n", b" \n", 1))
        commit(repo, "byte in " + rel)
        code, text = compute(repo)
        now = line_of(text, "game=")
        check(code == 0 and now is not None and now not in seen, f"source id: a byte changed in {rel} moves the game line: exit {code}\n{text}")
        want, _ = oracle_game(repo)
        check(now == f"game={want}", f"source id after {rel}: equals the oracle: {now} vs {want}")
        seen.add(now)
    # a new file under src/ moves it too
    write(os.path.join(repo, "src", "new.cpp"), b"int n;\n")
    commit(repo, "new file")
    code, text = compute(repo)
    check(code == 0 and line_of(text, "game=") not in seen, f"source id: a new file under src/ moves the game line\n{text}")


def test_pgo_none_when_absent(root):
    repo = make_repo(root)
    sdk = make_repo(root, "sdk", {"a.txt": b"sdk\n"})
    for how in ("pin", "worktree:" + sdk):
        code, text = pgo("check", "--repo", repo, "--sdk-from", how)
        check(code == 0 and text.strip() == "pgo: none", f"check --sdk-from {how}, no pgo/ files: exit 0, 'pgo: none': exit {code}\n{text!r}")


def test_pgo_refuses_half(root):
    repo = make_repo(root)
    code, text = compute(repo)
    put_pgo(repo, source=None, profile=True)
    code, text = pgo("check", "--repo", repo, "--sdk-from", "pin")
    check(code == 1 and "pgo: refused: profile without source.txt" in text and "pgo: use" not in text,
          f"check, profile alone: refused: exit {code}\n{text}")
    os.remove(os.path.join(repo, "pgo", "doa4.profdata"))
    c, src = compute(repo)
    put_pgo(repo, source=src, profile=False)
    code, text = pgo("check", "--repo", repo, "--sdk-from", "pin")
    check(code == 1 and "pgo: refused: source.txt without profile" in text and "pgo: use" not in text,
          f"check, source.txt alone (a matching one): refused: exit {code}\n{text}")


def test_pgo_mismatch_refuses(root):
    repo = make_repo(root)
    code, src = compute(repo)
    put_pgo(repo, source=src)
    code, text = pgo("check", "--repo", repo, "--sdk-from", "pin")
    check(code == 0 and text.strip() == use_line(repo), f"check, matching files: exit 0, '{use_line(repo)}': exit {code}\n{text!r}")
    # source.txt with CRLF (a committed file in a CRLF checkout) reads the same
    put_pgo(repo, source=src.replace("\n", "\r\n"))
    code, text = pgo("check", "--repo", repo, "--sdk-from", "pin")
    check(code == 0 and text.strip() == use_line(repo), f"check, CRLF source.txt: still used: exit {code}\n{text!r}")
    # a fix lands after the profile was made (Review Focus 2): committed, so the tree is clean and the game differs
    write(os.path.join(repo, "src", "x.cpp"), b"int x = 10;\n")
    commit(repo, "hot fix")
    code, text = pgo("check", "--repo", repo, "--sdk-from", "pin")
    check(code == 1 and "pgo: refused: game tree differs" in text and "working tree" not in text and "pgo: use" not in text,
          f"check after a committed fix: refused, game tree differs: exit {code}\n{text}")
    # llvm differs, with the game and the sdk right
    code, src2 = compute(repo)
    put_pgo(repo, source=src2.replace("llvm=23.1.4", "llvm=23.1.3"))
    code, text = pgo("check", "--repo", repo, "--sdk-from", "pin")
    check(code == 1 and "pgo: refused: llvm differs" in text and "pgo: use" not in text, f"check, llvm=23.1.3: refused: exit {code}\n{text}")
    # a source.txt without its sdk line is refused, never read as a match
    put_pgo(repo, source="".join(l + "\n" for l in src2.splitlines() if not l.startswith("sdk=")))
    code, text = pgo("check", "--repo", repo, "--sdk-from", "pin")
    check(code == 1 and "pgo: refused:" in text and "pgo: use" not in text and "Traceback" not in text,
          f"check, source.txt without sdk=: refused: exit {code}\n{text}")
    # the naming mode (names review F1): a profile from before the names line (game, sdk and llvm all matching) and one
    # made in the other mode are refused; a profile made in one mode gives a build in the other mode nothing, silently
    put_pgo(repo, source="".join(l + "\n" for l in src2.splitlines() if not l.startswith("names=")))
    code, text = pgo("check", "--repo", repo, "--sdk-from", "pin")
    check(code == 1 and "pgo: refused: source.txt malformed: no names line" in text and "pgo: use" not in text,
          f"check, a three-line source.txt (made before the names line) with game, sdk and llvm right: refused: exit {code}\n{text}")
    for other in ("path", "basename"):  # basename: a profile made with the names flag but without the macro prefix maps
        put_pgo(repo, source=src2.replace("names=portable", "names=" + other))
        code, text = pgo("check", "--repo", repo, "--sdk-from", "pin")
        check(code == 1 and "pgo: refused: names differ" in text and "pgo: use" not in text, f"check, names={other}: refused: exit {code}\n{text}")
    put_pgo(repo, source=src2)
    code, text = pgo("check", "--repo", repo, "--sdk-from", "pin")
    check(code == 0 and text.strip() == use_line(repo), f"check, the same four lines again: used: exit {code}\n{text!r}")


def test_pgo_sdk_differs(root):
    repo = make_repo(root)
    # pin: netplay_sdk.txt names SDK_A
    code, src = compute(repo, sdk=SDK_B)
    put_pgo(repo, source=src)
    code, text = pgo("check", "--repo", repo, "--sdk-from", "pin")
    check(code == 1 and "pgo: refused: sdk differs" in text and "pgo: use" not in text, f"check, sdk={SDK_B[:8]} vs pin {SDK_A[:8]}: refused: exit {code}\n{text}")
    write(os.path.join(repo, "netplay_sdk.txt"), f"repo=https://example.invalid/sdk\ncommit={SDK_B}\n".encode("ascii"))
    commit(repo, "pin moves")
    code, text = pgo("check", "--repo", repo, "--sdk-from", "pin")
    check(code == 0 and text.strip() == use_line(repo), f"check, pin moved to the profile's sdk: used: exit {code}\n{text}")
    # worktree:PATH reads that repository's HEAD
    sdk = make_repo(root, "sdk", {"a.txt": b"sdk\n"})
    h = head(sdk)
    code, src = compute(repo, sdk=h)
    put_pgo(repo, source=src)
    code, text = pgo("check", "--repo", repo, "--sdk-from", "worktree:" + sdk)
    check(code == 0 and text.strip() == use_line(repo), f"check --sdk-from worktree, HEAD {h[:8]}: used: exit {code}\n{text}")
    code, text = pgo("check", "--repo", repo, "--sdk-from", "pin")
    check(code == 1 and "pgo: refused: sdk differs" in text, f"check --sdk-from pin, profile made against the worktree: refused: exit {code}\n{text}")
    write(os.path.join(sdk, "a.txt"), b"sdk moved\n")
    commit(sdk, "sdk moves")
    code, text = pgo("check", "--repo", repo, "--sdk-from", "worktree:" + sdk)
    check(code == 1 and "pgo: refused: sdk differs" in text and "pgo: use" not in text, f"check --sdk-from worktree after an SDK commit: refused: exit {code}\n{text}")


def test_pgo_dirty_tree_refused(root):
    repo = make_repo(root)
    code, src = compute(repo)
    put_pgo(repo, source=src)
    write(os.path.join(repo, "src", "x.cpp"), b"int x = 11;\n")  # edited, not committed: HEAD's id would match
    code, text = compute(repo)
    check(code == 1 and "pgo: refused: working tree differs in src/x.cpp" in text and "game=" not in text,
          f"compute, src/x.cpp edited: refused: exit {code}\n{text}")
    code, text = pgo("check", "--repo", repo, "--sdk-from", "pin")
    check(code == 1 and "pgo: refused: working tree differs in src/x.cpp" in text and "pgo: use" not in text,
          f"check, src/x.cpp edited, matching files: refused: exit {code}\n{text}")
    git(repo, "checkout", "--", "src/x.cpp")
    write(os.path.join(repo, "hints.toml"), b"[hints]\na = 9\n")
    code, text = pgo("check", "--repo", repo, "--sdk-from", "pin")
    check(code == 1 and "working tree differs in hints.toml" in text, f"check, hints.toml edited: refused: exit {code}\n{text}")
    git(repo, "checkout", "--", "hints.toml")
    write(os.path.join(repo, "src", "new.cpp"), b"int n;\n")  # untracked under src/
    code, text = pgo("check", "--repo", repo, "--sdk-from", "pin")
    check(code == 1 and "working tree differs in src/new.cpp" in text, f"check, untracked src/new.cpp: refused: exit {code}\n{text}")
    os.remove(os.path.join(repo, "src", "new.cpp"))
    # edits outside the seven paths do not count
    write(os.path.join(repo, "tools", "y.py"), b"print(3)\n")
    write(os.path.join(repo, "notes.txt"), b"untracked\n")
    code, text = pgo("check", "--repo", repo, "--sdk-from", "pin")
    check(code == 0 and text.strip() == use_line(repo), f"check, edits outside the paths only: used: exit {code}\n{text}")
    # no profile at all: a working tree under development configures plainly (nothing is compiled from the id)
    shutil.rmtree(os.path.join(repo, "pgo"))
    write(os.path.join(repo, "src", "x.cpp"), b"int x = 12;\n")
    code, text = pgo("check", "--repo", repo, "--sdk-from", "pin")
    check(code == 0 and text.strip() == "pgo: none", f"check, no pgo/ files and src edited: 'pgo: none': exit {code}\n{text}")


def workflow_text():
    return read(WORKFLOW).decode("utf-8").replace("\r\n", "\n")


def workflow_paths(text):
    m = re.search(r"^\s+paths:\s*\[(.*?)\]", text, re.M | re.S)
    return re.findall(r"'([^']+)'", m.group(1)) if m else []


def configure_step(text):
    m = re.search(r"- name: Configure, build, host tests\n(.*?)(?=\n      - name: |\Z)", text, re.S)
    return [l.strip() for l in m.group(1).split("\n")] if m else []


def test_pgo_workflow_wiring(root):
    text = workflow_text()
    check("pgo/**" in workflow_paths(text), f"workflow: paths: has 'pgo/**': {workflow_paths(text)}")
    check("docs/PGO.md" in text, "workflow: a comment names docs/PGO.md")
    rule = text.split("# PUBLISH RULE", 1)[-1].split("\non:", 1)[0] if "# PUBLISH RULE" in text else ""
    check("pgo/" in rule, f"workflow: the PUBLISH RULE comment lists pgo/ among the game's inputs: {rule!r}")
    lines = configure_step(text)
    i_check = next((i for i, l in enumerate(lines) if "tools/pgo_source_id.py check --sdk-from pin" in l), None)
    i_cmake = next((i for i, l in enumerate(lines) if l.startswith("cmake --preset")), None)
    check(i_check is not None and i_cmake is not None and i_check < i_cmake,
          f"workflow Configure: the check runs before cmake: check line {i_check}, cmake line {i_cmake}")
    if i_check is None or i_cmake is None or i_check >= i_cmake:
        return
    # the check's exit code is caught on the next line and stops the job before cmake
    m = re.fullmatch(r'set "(\w+)=%errorlevel%"', lines[i_check + 1])
    rc = m.group(1) if m else "?"
    check(m is not None, f"workflow Configure: the line after the check keeps its exit code: {lines[i_check + 1]!r}")
    check(f'if not "%{rc}%"=="0" exit /b 1' in lines[i_check + 1:i_cmake], f"workflow Configure: a non-zero check stops the job before cmake")
    # the profile flag: one line in the whole workflow, gated on 'pgo: use', setting a variable reset before it
    flag_lines = [l.strip() for l in text.split("\n") if "-fprofile-use" in l]
    check(len(flag_lines) == 1 and '"pgo: use"' in flag_lines[0], f"workflow: -fprofile-use only in one line, gated on 'pgo: use': {flag_lines}")
    m = re.search(r'set "(\w+)= -fprofile-use=%%c' + re.escape(" " + NAMES + MAPS_WF) + '"', flag_lines[0]) if flag_lines else None
    var = m.group(1) if m else "?"
    i_flag = lines.index(flag_lines[0]) if flag_lines and flag_lines[0] in lines else None
    check(m is not None and i_flag is not None and i_check < i_flag < i_cmake, f"workflow Configure: the flag line sits between the check and cmake: {i_flag}")
    check(f'set "{var}="' in lines[i_check:i_flag or 0], f"workflow Configure: {var} is cleared before the flag line")
    cm = lines[i_cmake]
    check(f'"-DCMAKE_C_FLAGS=-march=x86-64-v2%{var}%"' in cm and f'"-DCMAKE_CXX_FLAGS=-march=x86-64-v2%{var}%"' in cm,
          f"workflow Configure: cmake passes %{var}% in both C and C++ flags: {cm}")
    check("profile" not in cm, f"workflow Configure: the cmake line names no profile itself: {cm}")


def test_pgo_paths_list_agrees(root):
    ids = load("pgo_source_id")
    listed = workflow_paths(workflow_text())
    want = sorted(p[:-3] if p.endswith("/**") else p for p in listed if p not in ("pgo/**", "netplay_sdk.txt"))
    check(len(listed) >= 9 and "netplay_sdk.txt" in listed, f"paths agree: the workflow's paths: list was read: {listed}")
    check(sorted(ids.ID_PATHS) == want, f"paths agree: pgo_source_id.ID_PATHS {sorted(ids.ID_PATHS)} == the workflow's {want}")
    for p in ids.ID_PATHS:
        check(os.path.exists(os.path.join(REPO, p)), f"paths agree: {p} exists in this repository")


def test_pgo_gitattributes_binary(root):
    path = os.path.join(REPO, ".gitattributes")
    lines = read(path).decode("ascii").replace("\r\n", "\n").split("\n") if os.path.isfile(path) else []
    check("pgo/*.profdata binary" in lines, f".gitattributes: the line 'pgo/*.profdata binary': {lines}")
    code, text = git(REPO, "check-attr", "binary", "diff", "text", "--", "pgo/doa4.profdata")
    check(code == 0 and "binary: set" in text and "text: unset" in text and "diff: unset" in text,
          f"git check-attr on pgo/doa4.profdata: binary (no text conversion, no diff): exit {code}\n{text}")


def test_pgo_np_build_wiring(root):
    text = read(NP_BUILD).decode("ascii").replace("\r\n", "\n")
    lines = text.split("\n")
    cm = [l for l in lines if l.startswith("cmake --preset")]
    check(len(cm) == 1, f"np_build.bat: one configure line: {cm}")
    cm = cm[0] if cm else ""
    # every configure passes all four flag variables, so none keeps a cached value from an earlier configure
    for name in ("CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS", "CMAKE_EXE_LINKER_FLAGS", "CMAKE_SHARED_LINKER_FLAGS"):
        check(f'"-D{name}=' in cm, f"np_build.bat: the configure line passes {name}: {cm}")
    i_cm = lines.index(cm) if cm in lines else -1
    i_check = next((i for i, l in enumerate(lines) if 'pgo_source_id.py" check --sdk-from "worktree:%DOA4_SDK%"' in l), None)
    check(i_check is not None and i_check < i_cm, f"np_build.bat: the check runs before cmake (lines {i_check}, {i_cm})")
    flag_lines = [l for l in lines if "-fprofile-use" in l]
    check(len(flag_lines) == 1 and '"pgo: use"' in flag_lines[0], f"np_build.bat: -fprofile-use only in one line gated on 'pgo: use': {flag_lines}")
    for needle in ("-fprofile-generate=", "-DDOA4_PGO_DUMP", "-Wl,/force:unresolved", "-Wl,/export:__llvm_profile_dump"):
        check(needle in text, f"np_build.bat: the instrumented mode passes {needle}")
    # the profile names of file-local functions carry no source path in either mode (docs/PGO.md), so a build at
    # another path (CI) finds them: the flag sits in the gen line and in the use line, and nowhere else
    ids = load("pgo_source_id")
    check(getattr(ids, "NAMES_FLAG", None) == NAMES and getattr(ids, "MACRO_ROOTS", None) == ("/doa4", "/rexglue-sdk")
          and getattr(ids, "NAMES_MODE", None) == "portable",
          f"pgo_source_id.py: NAMES_FLAG is the scripts' flag, MACRO_ROOTS their map targets, NAMES_MODE portable: "
          f"{getattr(ids, 'NAMES_FLAG', None)!r}, {getattr(ids, 'MACRO_ROOTS', None)!r}, {getattr(ids, 'NAMES_MODE', None)!r}")
    # the anonymous-namespace hash follows the source file's name: both profile lines map the game and SDK roots
    root_lines = [l for l in lines if l.startswith('set "PGO_ROOT=')]
    check(root_lines == ['set "PGO_ROOT=%CD:\\=/%"'] and lines.index(root_lines[0]) > lines.index('cd /d "%~dp0.."'),
          f"np_build.bat: PGO_ROOT is the worktree's folder with forward slashes, set after the cd: {root_lines}")
    gen_lines = [l for l in lines if "-fprofile-generate=" in l]
    names_lines = [l for l in lines if NAMES in l and not l.lower().startswith("rem ")]  # commands, not the comment
    check(len(gen_lines) == 1 and len(flag_lines) == 1 and sorted(names_lines) == sorted(gen_lines + flag_lines),
          f"np_build.bat: {NAMES!r} in exactly the gen and the use line: gen {gen_lines}, use {flag_lines}, with it {names_lines}")
    maps_lines = [l for l in lines if MAPS_BAT.strip() in l and not l.lower().startswith("rem ")]
    check(len(gen_lines) == 1 and len(flag_lines) == 1 and sorted(maps_lines) == sorted(gen_lines + flag_lines)
          and all(l.rstrip().endswith(NAMES + MAPS_BAT + '"') for l in maps_lines),
          f"np_build.bat: the gen and the use line end with {NAMES + MAPS_BAT!r}: {maps_lines}")
    check(not any(l.rstrip().endswith("(") for l in lines), "np_build.bat: no parenthesised block (no errorlevel read inside one)")


def check_no_v3_build_route():
    """No build route may name the AVX2 CPU level; the exemptions are the text-keyed np_core.cpp:571 note and tests/host/suitconv_fp_control.cpp."""
    needle = "x86-64-v" + "3"  # built at run time so this file does not match itself
    r = subprocess.run(["git", "grep", "-n", "-I", needle, "--", ".", ":!thirdparty", ":!generated"],
                       cwd=REPO, capture_output=True, text=True)
    # git grep exits 0 (hits) or 1 (none); anything else, or any stderr, means the search did not run
    check(r.returncode in (0, 1) and not r.stderr.strip(), f"git grep ran: rc {r.returncode}, stderr {r.stderr.strip()!r}")
    # one table: file -> text a hit must hold to be exempt (None: every hit in the file). np_core.cpp is keyed to its
    # measurement comment, so a new v3 line elsewhere in it is caught; suitconv_fp_control.cpp is the FMA control (dev PC only).
    exempt = {"src/net/np_core.cpp": "measured with -march=" + needle, "tests/host/suitconv_fp_control.cpp": None}

    def is_exempt(line):
        path, _, rest = line.partition(":")
        if path not in exempt:
            return False
        return exempt[path] is None or exempt[path] in rest

    lines = r.stdout.splitlines()
    exempt_found = sorted({l.split(":")[0] for l in lines if is_exempt(l)})
    check(exempt_found == sorted(exempt),
          f"positive control: the raw search output holds a hit for each exempt entry (the search found the tree): {exempt_found}")
    hits = [l for l in lines if not is_exempt(l)]
    check(not hits, f"no {needle} build route outside the exempt entries: {hits}")


def test_no_v3_build_route(root):
    check_no_v3_build_route()



# ---- the wiring under real cmd: the workflow step's own lines and np_build.bat configure, cmake replaced by a recorder

WF_STEP = "Configure, build, host tests"
WF_VSDEVCMD = r'set "DOA4_VSDEVCMD=%VS%\Common7\Tools\VsDevCmd.bat"'
BAT_VSDEVCMD = r'"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat"'
PLAIN = "-march=x86-64-v2"
SHLINK = "-Wl,/force:unresolved -Wl,/export:__llvm_profile_dump"
GUARD = "pgo: use line read but no flag set"
NAMES = "-mllvm -static-func-full-module-prefix=false"  # path-free profile names for file-local functions
# path-free anonymous-namespace names: the game and SDK roots mapped to fixed tokens (the script text and the workflow text)
MAPS_BAT = " -fmacro-prefix-map=%PGO_ROOT%=/doa4 -fmacro-prefix-map=%DOA4_SDK%=/rexglue-sdk"
MAPS_WF = (" -fmacro-prefix-map=%GITHUB_WORKSPACE:\\=/%/doa4-recomp=/doa4"
           " -fmacro-prefix-map=%GITHUB_WORKSPACE:\\=/%/rexglue-sdk=/rexglue-sdk")


def maps_want(game_root, sdk_root):
    """The two map flags as cmake receives them, for a game and an SDK root with forward slashes."""
    return f" -fmacro-prefix-map={game_root}=/doa4 -fmacro-prefix-map={sdk_root}=/rexglue-sdk"


def step_run_lines(text, step):
    """The 'run: |' block of the named workflow step, dedented: the lines the runner writes into its .cmd file."""
    lines = text.split("\n")
    i = next((k for k, l in enumerate(lines) if l.strip() == f"- name: {step}"), None)
    j = next((k for k in range(i + 1, len(lines)) if lines[k].strip() == "run: |"), None) if i is not None else None
    if j is None:
        return []
    key = len(lines[j]) - len(lines[j].lstrip())
    body = []
    for l in lines[j + 1:]:
        if l.strip() and len(l) - len(l.lstrip()) <= key:
            break
        body.append(l)
    while body and not body[-1].strip():
        body.pop()
    ind = min((len(l) - len(l.lstrip()) for l in body if l.strip()), default=0)
    return [l[ind:] for l in body]


def make_stubs(root):
    """cmake.cmd (first on PATH) records its arguments one per line through Python's own argv parsing (what cmake.exe
    would get); VsDevCmd.bat does nothing. A .cmd called without CALL ends its caller, so nothing after cmake runs."""
    d = os.path.join(root, "stubs")
    os.makedirs(d)
    rec_py = os.path.join(d, "cmake_rec.py")
    with open(rec_py, "w", encoding="ascii", newline="\n") as f:
        f.write("import os, sys\n"
                "with open(os.environ['DOA4_TEST_CMAKE_REC'], 'a', encoding='utf-8', newline=chr(10)) as f:\n"
                "    f.write(''.join(a + chr(10) for a in sys.argv[1:]) + '--end of call--' + chr(10))\n")
    with open(os.path.join(d, "cmake.cmd"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(f'@"{sys.executable}" -I -B "{rec_py}" %*\n')
    vsdev = os.path.join(d, "VsDevCmd.bat")
    with open(vsdev, "w", encoding="ascii", newline="\r\n") as f:
        f.write("@exit /b 0\n")
    return d, vsdev


def recorded_calls(rec):
    if not os.path.isfile(rec):
        return None
    return [c.split("\n")[:-1] for c in read(rec).decode("utf-8").split("--end of call--\n") if c]


def dflags(args):
    out = {}
    for a in args:
        if a.startswith("-D") and "=" in a:
            k, _, v = a[2:].partition("=")
            out[k] = v
    return out


def test_pgo_wiring_cmd(root):
    check(os.name == "nt", "wiring under cmd: needs Windows cmd (CI is Windows); never skipped")
    if os.name != "nt":
        return
    root = os.path.realpath(root)  # long names (a runner's TEMP can be an 8.3 path)
    stubs, vsdev = make_stubs(root)
    # the workflow side: the step's lines read from the workflow file, up to its first cmake line
    body = step_run_lines(workflow_text(), WF_STEP)
    i_cm = next((i for i, l in enumerate(body) if l.startswith("cmake --preset")), None)
    n_vs = body.count(WF_VSDEVCMD)
    check(i_cm is not None and n_vs == 1, f"wiring fixture: the step has a cmake --preset line ({i_cm}) and one VsDevCmd line ({n_vs}): {body}")
    if i_cm is None or n_vs != 1:
        return
    body = [f'set "DOA4_VSDEVCMD={vsdev}"' if l == WF_VSDEVCMD else re.sub(r"\$\{\{[^}]*\}\}", "x", l) for l in body[:i_cm + 1]]
    # the bat side: a fixture repository with np_build.bat (its absolute VsDevCmd path pointed at the stub) and
    # pgo_source_id.py in tools/, the seven id paths from ID_FILES, and netplay_sdk.txt naming the fixture SDK's HEAD
    bat = read(NP_BUILD)
    n_bat = bat.count(BAT_VSDEVCMD.encode("ascii"))
    check(n_bat == 1, f"wiring fixture: np_build.bat calls VsDevCmd at its one absolute path: {n_bat}")
    if n_bat != 1:
        return
    bat = bat.replace(BAT_VSDEVCMD.encode("ascii"), f'"{vsdev}"'.encode("utf-8"))
    sdk = make_repo(root, "sdk", {"a.txt": b"sdk\n"})
    ws = os.path.join(root, "ws")
    files = {**ID_FILES, "tools/y.py": b"print(1)\n", "README.md": b"# readme\n",
             "netplay_sdk.txt": f"repo=https://example.invalid/sdk\ncommit={head(sdk)}\n".encode("ascii"),
             "tools/pgo_source_id.py": read(PGO_ID), "tools/np_build.bat": bat}
    repo = make_repo(ws, "doa4-recomp", files)
    fx_bat = os.path.join(repo, "tools", "np_build.bat")
    rt = os.path.join(root, "runner_temp")
    os.makedirs(rt)
    base = {k: v for k, v in os.environ.items() if not k.upper().startswith("PGO_") and k.upper() != "DOA4_PGO"}
    base.update(PATH=stubs + os.pathsep + os.path.dirname(sys.executable) + os.pathsep + os.environ.get("PATH", ""),
                RUNNER_TEMP=rt, GITHUB_WORKSPACE=r"D:\a\r\r", DOA4_SDK=sdk.replace(os.sep, "/"))
    n = [0]

    def run_side(side, extra=None, wf_body=None, bat_path=None):
        n[0] += 1
        rec = os.path.join(root, f"rec_{n[0]}.txt")
        env = dict(base, DOA4_TEST_CMAKE_REC=rec, **(extra or {}))
        if side == "workflow":
            script = os.path.join(root, f"wf_{n[0]}.cmd")
            with open(script, "w", encoding="utf-8", newline="\r\n") as f:
                f.write("\n".join(["@echo off", f'cd /d "{ws}"', *(wf_body or body)]) + "\n")
            line = f'cmd /D /E:ON /V:OFF /S /C "CALL "{script}""'
        else:
            line = f'cmd /D /E:ON /V:OFF /S /C "CALL "{bat_path or fx_bat}" configure"'
        try:
            p = subprocess.run(line, cwd=root, env=env, capture_output=True, text=True, encoding="utf-8",
                               errors="replace", timeout=180)
            code, out = p.returncode, p.stdout + p.stderr
        except subprocess.TimeoutExpired:
            code, out = "timeout", ""
        return code, out, recorded_calls(rec)

    def reached(what, res):
        code, out, calls = res
        ok = calls is not None and len(calls) == 1 and calls[0][:2] == ["--preset", "win-amd64-release"]
        check(ok and code == 0, f"wiring {what}: cmake reached once with --preset, exit 0: exit {code}, calls {calls}\n{out}")
        return (dflags(calls[0]), calls[0]) if ok else ({}, [])

    def stopped(what, res, text):
        code, out, calls = res
        check(calls is None, f"wiring {what}: cmake never reached: {calls}\n{out}")
        check(code not in (0, "timeout"), f"wiring {what}: exit code not 0: {code}\n{out}")
        check(text in out, f"wiring {what}: prints {text!r}:\n{out}")

    def plain(what, f, args, bat_side):
        check(f.get("CMAKE_C_FLAGS") == PLAIN and f.get("CMAKE_CXX_FLAGS") == PLAIN,
              f"wiring {what}: plain C and C++ flags: {f.get('CMAKE_C_FLAGS')!r}, {f.get('CMAKE_CXX_FLAGS')!r}")
        check(not any("-fprofile" in a or "-bogus" in a or "static-func" in a or "prefix-map" in a for a in args),
              f"wiring {what}: no -fprofile, no profile-name flag and nothing inherited anywhere: {args}")
        check(f.get("CMAKE_SHARED_LINKER_FLAGS", "") == "" and f.get("CMAKE_EXE_LINKER_FLAGS", "") == "",
              f"wiring {what}: linker flags empty: {f.get('CMAKE_SHARED_LINKER_FLAGS')!r}, {f.get('CMAKE_EXE_LINKER_FLAGS')!r}")
        if bat_side:
            check("CMAKE_SHARED_LINKER_FLAGS" in f and "CMAKE_EXE_LINKER_FLAGS" in f, f"wiring {what}: both linker flags passed (empty): {args}")

    sides = ("workflow", "np_build.bat")
    # none: no pgo/ folder; inherited: PGO_C / PGO_USE set in the environment, no pgo/ folder
    for side in sides:
        f, args = reached(f"{side} none", run_side(side))
        plain(f"{side} none", f, args, side != "workflow")
        f, args = reached(f"{side} inherited", run_side(side, {"PGO_C": " -bogus", "PGO_USE": " -bogus"}))
        plain(f"{side} inherited PGO_C/PGO_USE= -bogus", f, args, side != "workflow")
    # gen (np_build.bat only), an absolute folder with backslashes
    gen_dir = os.path.join(root, "prof")
    res = run_side("np_build.bat", {"DOA4_PGO": "gen:" + gen_dir})
    f, args = reached("np_build.bat gen", res)
    bat_maps = maps_want(repo.replace(os.sep, "/"), sdk.replace(os.sep, "/"))
    wf_maps = maps_want("D:/a/r/r/doa4-recomp", "D:/a/r/r/rexglue-sdk")  # GITHUB_WORKSPACE=D:\a\r\r above
    want = f"{PLAIN} -fprofile-generate={gen_dir.replace(os.sep, '/')} -DDOA4_PGO_DUMP {NAMES}{bat_maps}"
    check(f.get("CMAKE_C_FLAGS") == want and f.get("CMAKE_CXX_FLAGS") == want,
          f"wiring np_build.bat gen: C and C++ flags are {want!r}: {f.get('CMAKE_C_FLAGS')!r}, {f.get('CMAKE_CXX_FLAGS')!r}")
    check(f.get("CMAKE_SHARED_LINKER_FLAGS") == SHLINK, f"wiring np_build.bat gen: shared linker flags {SHLINK!r}: {f.get('CMAKE_SHARED_LINKER_FLAGS')!r}")
    check(f.get("CMAKE_EXE_LINKER_FLAGS") == "", f"wiring np_build.bat gen: exe linker flags passed empty: {f.get('CMAKE_EXE_LINKER_FLAGS')!r}")
    # use: matching pgo/ files (made against the fixture SDK's HEAD, which the pin names too)
    code, src = compute(repo, sdk=head(sdk))
    check(code == 0, f"wiring fixture: compute: exit {code}\n{src}")
    put_pgo(repo, source=src)
    profile = os.path.join(repo, "pgo", "doa4.profdata")
    for side in sides:
        res = run_side(side)
        f, args = reached(f"{side} use", res)
        m = re.search(r"^pgo: use (\S.*?)\s*$", res[1], re.M)
        path = m.group(1) if m else ""
        check(m is not None and "\\" not in path and os.path.isabs(path) and os.path.isfile(path) and os.path.samefile(path, profile),
              f"wiring {side} use: 'pgo: use <absolute path, forward slashes>' names the fixture's profile: {path!r}\n{res[1]}")
        want = f"{PLAIN} -fprofile-use={path} {NAMES}" + (wf_maps if side == "workflow" else bat_maps)
        check(bool(path) and f.get("CMAKE_C_FLAGS") == want and f.get("CMAKE_CXX_FLAGS") == want,
              f"wiring {side} use: C and C++ flags are {want!r}: {f.get('CMAKE_C_FLAGS')!r}, {f.get('CMAKE_CXX_FLAGS')!r}")
        check("-fprofile" not in f.get("CMAKE_SHARED_LINKER_FLAGS", "") + f.get("CMAKE_EXE_LINKER_FLAGS", ""),
              f"wiring {side} use: no profile flag in the linker flags: {args}")
        if side != "workflow":
            check(f.get("CMAKE_SHARED_LINKER_FLAGS") == "" and f.get("CMAKE_EXE_LINKER_FLAGS") == "", f"wiring {side} use: linker flags passed empty: {args}")
    # round-3 review R3-1: a caller's DOA4_SDK with backslashes and a trailing separator still gives the forward-slash
    # SDK map (clang matches the map against CMake's forward-slash compile paths and says nothing when it does not match)
    # (the third spelling is relative to the cmd's folder, root: made absolute before np_build.bat changes folder)
    for spelled in (sdk.replace("/", os.sep) + os.sep, sdk.replace(os.sep, "/") + "/", os.path.relpath(sdk, root) + os.sep):
        res = run_side("np_build.bat", {"DOA4_SDK": spelled})
        f, args = reached(f"np_build.bat use, DOA4_SDK={spelled!r}", res)
        m = re.search(r"^pgo: use (\S.*?)\s*$", res[1], re.M)
        want = f"{PLAIN} -fprofile-use={m.group(1) if m else '?'} {NAMES}{bat_maps}"
        check(f.get("CMAKE_C_FLAGS") == want and f.get("REXSDK_DIR") == sdk.replace(os.sep, "/"),
              f"wiring np_build.bat use, DOA4_SDK={spelled!r}: the map and REXSDK_DIR name the SDK in forward slashes, no trailing "
              f"slash: {f.get('CMAKE_C_FLAGS')!r}, REXSDK_DIR {f.get('REXSDK_DIR')!r}")
    # off (np_build.bat only), with the matching pgo/ files present: still plain
    res = run_side("np_build.bat", {"DOA4_PGO": "off"})
    f, args = reached("np_build.bat off", res)
    plain("np_build.bat off", f, args, True)
    check("pgo: off" in res[1], f"wiring np_build.bat off: prints 'pgo: off':\n{res[1]}")
    # the guard: the flag line dropped from a copy, so a 'pgo: use' line sets no flag: the job stops before cmake
    wf_flag = [i for i, l in enumerate(body) if "-fprofile-use=%%c" in l]
    bat_lines = bat.split(b"\r\n")
    bat_flag = [i for i, l in enumerate(bat_lines) if b"-fprofile-use=%%c" in l]
    check(len(wf_flag) == 1 and len(bat_flag) == 1, f"wiring fixture: one flag line each: {wf_flag}, {bat_flag}")
    if len(wf_flag) == 1 and len(bat_flag) == 1:
        noflag_body = body[:wf_flag[0]] + ["rem the flag line, dropped by the test"] + body[wf_flag[0] + 1:]
        stopped("workflow use, flag line dropped", run_side("workflow", wf_body=noflag_body), GUARD)
        noflag_bat = os.path.join(repo, "tools", "np_build_noflag.bat")
        write(noflag_bat, b"\r\n".join(bat_lines[:bat_flag[0]] + [b"rem the flag line, dropped by the test"] + bat_lines[bat_flag[0] + 1:]))
        stopped("np_build.bat use, flag line dropped", run_side("np_build.bat", bat_path=noflag_bat), GUARD)
        os.remove(noflag_bat)
    # half: the profile alone
    os.remove(os.path.join(repo, "pgo", "source.txt"))
    for side in sides:
        stopped(f"{side} half", run_side(side), "pgo: refused: profile without source.txt")
    # mismatch: a fix committed after the profile was made
    put_pgo(repo, source=src)
    write(os.path.join(repo, "src", "x.cpp"), b"int x = 10;\n")
    commit(repo, "hot fix")
    for side in sides:
        stopped(f"{side} mismatch", run_side(side), "pgo: refused: game tree differs")


def test_pgo_source_unreadable(root):
    """M1: a source.txt that exists but cannot be read is a refusal, never a traceback (a deny-read ACE, as the
    review's probe made it; a directory named source.txt never reaches the read: it is not a file)."""
    repo = make_repo(root)
    code, src = compute(repo)
    put_pgo(repo, source=src)
    path = os.path.join(repo, "pgo", "source.txt")
    user = os.environ.get("USERNAME", "")
    d = subprocess.run(["icacls", path, "/deny", f"{user}:(R)"], capture_output=True, text=True, errors="replace", timeout=60)
    check(d.returncode == 0, f"fixture: icacls /deny {user}:(R): exit {d.returncode}\n{d.stdout}{d.stderr}")
    try:
        try:
            with open(path, "rb") as f:
                f.read()
            readable = True
        except OSError:
            readable = False
        check(not readable, "fixture: source.txt with the deny-read ACE cannot be read here")
        code, text = pgo("check", "--repo", repo, "--sdk-from", "pin")
        check(code == 1 and "pgo: refused: source.txt could not be read:" in text and "Traceback" not in text and "pgo: use" not in text,
              f"check, unreadable source.txt: refused, no traceback: exit {code}\n{text}")
    finally:
        subprocess.run(["icacls", path, "/remove:d", user], capture_output=True, timeout=60)


def test_pgo_llvm_version_agrees(root):
    """M2: LLVM in pgo_source_id.py is the workflow's LLVM: its step's name and its download URL."""
    spec = importlib.util.spec_from_file_location("pgo_source_id_m2", PGO_ID)
    ids = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ids)
    text = workflow_text()
    names = re.findall(r"^\s*- name: LLVM (\d+\.\d+\.\d+)\s*$", text, re.M)
    urls = re.findall(r"llvmorg-(\d+\.\d+\.\d+)", text)
    check(len(names) == 1 and names[0] == ids.LLVM, f"LLVM: the workflow's one 'LLVM x.y.z' step names {ids.LLVM}: {names}")
    check(len(urls) >= 1 and all(u == ids.LLVM for u in urls), f"LLVM: the workflow's download URL is llvmorg-{ids.LLVM}: {urls}")


WAY_OUT = "(to build without the profile: DOA4_PGO=off locally; in CI delete pgo/doa4.profdata and pgo/source.txt)"


def test_pgo_refusal_way_out(root):
    """M4: every refusal printed by check ends with the way out, and BUILDING.md section 6 says it too."""
    repo = make_repo(root)
    code, src = compute(repo)
    cases = []
    put_pgo(repo, source=None)
    cases.append(("profile alone", pgo("check", "--repo", repo, "--sdk-from", "pin")))
    os.remove(os.path.join(repo, "pgo", "doa4.profdata"))
    put_pgo(repo, source=src, profile=False)
    cases.append(("source.txt alone", pgo("check", "--repo", repo, "--sdk-from", "pin")))
    put_pgo(repo, source=src)
    cases.append(("bad --sdk-from", pgo("check", "--repo", repo, "--sdk-from", "nowhere")))
    put_pgo(repo, source=src.replace("llvm=23.1.4", "llvm=23.1.3"))
    cases.append(("llvm", pgo("check", "--repo", repo, "--sdk-from", "pin")))
    put_pgo(repo, source="game=1\n")
    cases.append(("malformed", pgo("check", "--repo", repo, "--sdk-from", "pin")))
    put_pgo(repo, source=src.replace(SDK_A, SDK_B))
    cases.append(("sdk", pgo("check", "--repo", repo, "--sdk-from", "pin")))
    put_pgo(repo, source=src)
    write(os.path.join(repo, "src", "x.cpp"), b"int x = 11;\n")
    cases.append(("dirty", pgo("check", "--repo", repo, "--sdk-from", "pin")))
    commit(repo, "hot fix")
    cases.append(("game", pgo("check", "--repo", repo, "--sdk-from", "pin")))
    for what, (code, text) in cases:
        lines = [l for l in text.splitlines() if l.startswith("pgo: refused:")]
        check(code == 1 and len(lines) == 1 and lines[0].endswith(" " + WAY_OUT),
              f"check refusal ({what}) ends with the way out: exit {code}\n{text}")
    building = read(os.path.join(REPO, "docs", "BUILDING.md")).decode("utf-8")
    s6 = building.split("## 6. Build", 1)[-1].split("## 7.", 1)[0]
    check("DOA4_PGO=off" in s6 and "pgo/doa4.profdata" in s6 and "pgo/source.txt" in s6,
          "docs/BUILDING.md section 6: a refusal's way out (DOA4_PGO=off; in CI delete both pgo/ files)")


def test_pgo_docs(root):
    doc = os.path.join(REPO, "docs", "PGO.md")
    text = read(doc).decode("utf-8").replace("\r\n", "\n") if os.path.isfile(doc) else ""
    check(0 < text.count("\n") < 60, f"docs/PGO.md: exists, under 60 lines: {text.count(chr(10))}")
    for needle in ("tools/pgo_recipe.py", "pgo/source.txt", "pgo/doa4.profdata", "DOA4_PGO",
                   "out/build/win-amd64-release", "ninja", "HEAD commit", NAMES, "names=portable", "-fmacro-prefix-map"):
        check(needle in text, f"docs/PGO.md names {needle}")
    building = read(os.path.join(REPO, "docs", "BUILDING.md")).decode("utf-8")
    s6 = building.split("## 6. Build", 1)[-1].split("## 7.", 1)[0]
    check("PGO.md" in s6, "docs/BUILDING.md section 6 links docs/PGO.md")


GOOD_LOG = "12:00:00.1 quit at frame 4000: quit frame\n12:00:00.2 pgo dump: exe 0, rexruntime.dll 0\n"


def test_pgo_recipe_run_judge(root):
    rc = load("pgo_recipe")
    judge = rc.judge_run
    ok, why = judge(0, "status ok\nexit 0 after 74 s\n", GOOD_LOG, 2)
    check(ok is True, f"run judge: a good run is good: {why}")
    ok, why = judge(0, "status ok\r\nexit 0\r\n", GOOD_LOG.replace("\n", "\r\n"), 2)
    check(ok is True, f"run judge: CRLF files read the same: {why}")
    bad = [((1, "status ok\n", GOOD_LOG, 2), "exit"),
           ((0, "status failed\n", GOOD_LOG, 2), "status"),
           ((0, "note\nstatus ok\n", GOOD_LOG, 2), "status"),
           ((0, None, GOOD_LOG, 2), "status"),
           ((0, "status ok\n", GOOD_LOG.replace("rexruntime.dll 0", "rexruntime.dll -101"), 2), "dump"),
           ((0, "status ok\n", GOOD_LOG.replace("rexruntime.dll 0", "rexruntime.dll -100"), 2), "dump"),
           ((0, "status ok\n", GOOD_LOG.replace("exe 0,", "exe 3,"), 2), "dump"),
           ((0, "status ok\n", GOOD_LOG.replace("rexruntime.dll 0", "rexruntime.dll 01"), 2), "dump"),
           ((0, "status ok\n", "12:00:00.1 quit at frame 4000: quit frame\n", 2), "dump"),
           ((0, "status ok\n", None, 2), "dump"),
           ((0, "status ok\n", GOOD_LOG, 1), "profraw"),
           ((0, "status ok\n", GOOD_LOG, 3), "profraw"),
           ((0, "status ok\n", GOOD_LOG, 0), "profraw")]
    for args, token in bad:
        ok, why = judge(*args)
        check(ok is False and token in why, f"run judge {args[0]}, {args[1]!r}, dump line {('dump' in (args[2] or ''))}, {args[3]} profraw: refused naming {token!r}: {ok} {why!r}")
    # source.txt text: compute's four lines, normalised to LF; anything else refused
    good = f"game={'a' * 64}\nsdk={SDK_B}\nllvm=23.1.4\nnames=portable\n"
    check(rc.source_text(good.replace("\n", "\r\n")) == good, "recipe: source.txt text from compute's CRLF output is LF")
    for wrong in (good.replace("llvm=23.1.4\n", ""), good.replace("a" * 64, "a" * 63), "pgo: refused: x\n", good + "x=1\n",
                  good.replace("names=portable\n", ""), good.replace("names=portable", "names=path"),
                  good.replace("names=portable", "names=basename")):
        try:
            rc.source_text(wrong)
            got = "accepted"
        except ValueError as e:
            got = f"ValueError: {e}"
        check(got.startswith("ValueError"), f"recipe: source.txt text refused for {wrong!r}: {got}")


def recipe_cli(root, toml_text, stray=False):
    out = os.path.join(root, "out")
    if stray:
        write(os.path.join(out, "prof", "left.profraw"), b"old")
    toml = os.path.join(root, "dead_or_alive_4.toml")
    write(toml, toml_text.encode("ascii"))
    wt = os.path.join(root, "not a worktree")
    os.makedirs(wt)
    code, text = run(PGO_RECIPE, "--worktree", wt, "--sdk", os.path.join(root, "no sdk"), "--out", out,
                     "--tag-prefix", "t1", "--toml", toml, "--userdata", os.path.join(root, "ud"))
    log = read(os.path.join(out, "recipe.log")).decode("utf-8") if os.path.isfile(os.path.join(out, "recipe.log")) else ""
    return code, text, log, out


def test_pgo_recipe_stray_profraw(root):
    rc = load("pgo_recipe")
    prof = os.path.join(root, "prof")
    check(rc.stray_profraw(prof) == [], "stray: a missing folder has none")
    os.makedirs(prof)
    check(rc.stray_profraw(prof) == [], "stray: an empty folder has none")
    write(os.path.join(prof, "tag", "default_1_0.profraw"), b"moved")
    write(os.path.join(prof, "notes.profraw.txt"), b"not one")
    check(rc.stray_profraw(prof) == [], f"stray: files moved into a script's folder are not stray: {rc.stray_profraw(prof)}")
    write(os.path.join(prof, "default_2_0.profraw"), b"left")
    check(rc.stray_profraw(prof) == ["default_2_0.profraw"], f"stray: a .profraw in the folder itself is: {rc.stray_profraw(prof)}")
    code, text, log, out = recipe_cli(root, "audio_mute = true\n", stray=True)
    last = log.strip().split("\n")[-1] if log.strip() else ""
    check(code == 1 and "recipe stopped:" in last and "left.profraw" in last, f"recipe with a stray .profraw: stopped before anything: exit {code}\n{log}\n{text}")
    check(sorted(os.listdir(out)) == ["prof", "recipe.log"], f"recipe with a stray .profraw: nothing else written: {sorted(os.listdir(out))}")


def test_pgo_recipe_toml_unmuted_refused(root):
    rc = load("pgo_recipe")
    check(rc.toml_muted("a = 1\naudio_mute = true\n") and rc.toml_muted("audio_mute = true\r\n"), "toml: audio_mute = true is muted (LF and CRLF)")
    for text in ("audio_mute = false\n", "# audio_mute = true\n", "audio_mute = truer\n", "", "x_audio_mute = true\n"):
        check(not rc.toml_muted(text), f"toml: {text!r} is not muted")
    code, text, log, out = recipe_cli(root, "audio_mute = false\n")
    last = log.strip().split("\n")[-1] if log.strip() else ""
    check(code == 1 and "recipe stopped:" in last and "audio_mute = true" in last, f"recipe, unmuted toml: stopped: exit {code}\n{log}\n{text}")
    check(re.match(r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d ", last) is not None, f"recipe.log lines carry their time: {last!r}")
    check(sorted(os.listdir(out)) == ["recipe.log"], f"recipe, unmuted toml: nothing else written: {sorted(os.listdir(out))}")


def test_pgo_recipe_reused_out(root):
    """M7a: preflight refuses an --out folder an earlier recipe used: runs/ exists, or a .profraw anywhere under prof/."""
    rc = load("pgo_recipe")
    out = os.path.join(root, "o")
    check(rc.reused_out(out) is None, "reused out: a missing folder is fresh")
    write(os.path.join(out, "prof", "notes.txt"), b"x")
    write(os.path.join(out, "prof", "tag", "readme.txt"), b"x")
    check(rc.reused_out(out) is None, f"reused out: prof/ without .profraw is fresh: {rc.reused_out(out)}")
    write(os.path.join(out, "prof", "tag", "default_1_0.profraw"), b"old")
    why = rc.reused_out(out) or ""
    check("default_1_0.profraw" in why and os.path.join("prof", "tag") in why, f"reused out: a .profraw in prof/tag/ is named: {why!r}")
    shutil.rmtree(os.path.join(out, "prof"))
    os.makedirs(os.path.join(out, "runs"))
    why = rc.reused_out(out) or ""
    check(os.path.join(out, "runs") in why, f"reused out: an existing runs/ is named: {why!r}")
    # through the CLI: stopped before anything else, the reason in recipe.log's last line
    os.makedirs(os.path.join(root, "out", "runs"))
    code, text, log, out2 = recipe_cli(root, "audio_mute = true\n")
    last = log.strip().split("\n")[-1] if log.strip() else ""
    check(code == 1 and "recipe stopped:" in last and os.path.join(out2, "runs") in last, f"recipe with an existing runs/: stopped naming it: exit {code}\n{log}\n{text}")
    check(sorted(os.listdir(out2)) == ["recipe.log", "runs"], f"recipe with an existing runs/: nothing else written: {sorted(os.listdir(out2))}")


def test_pgo_recipe_snapshot_games(root):
    """M7b: after a timeout only games started from the instrumented snapshot are killed, never another session's."""
    rc = load("pgo_recipe")
    snap = os.path.join(root, "wt", "out", "build", "win-amd64-release", "np_bin", "pgI")
    text = ("101|" + os.path.join(snap, "dead_or_alive_4.exe") + "\r\n"
            "102|" + os.path.join(root, "wt-lp", "out", "np_bin", "lp-rc1", "dead_or_alive_4.exe") + "\r\n"
            "103|\r\n"
            "\r\n"
            "104|" + os.path.join(snap + "2", "dead_or_alive_4.exe") + "\n"
            "105|" + snap.upper().replace(os.sep, "/") + "/DEAD_OR_ALIVE_4.EXE\n"
            "106|" + os.path.join(snap, "TracyClient.exe") + "\n"
            "107|" + os.path.join(snap, "sub", "dead_or_alive_4.exe") + "\n")
    procs = rc.parse_procs(text)
    check(procs[:3] == [(101, os.path.join(snap, "dead_or_alive_4.exe")),
                        (102, os.path.join(root, "wt-lp", "out", "np_bin", "lp-rc1", "dead_or_alive_4.exe")), (103, None)]
          and len(procs) == 7, f"parse_procs: pid and image path per line, None for no path, blank lines skipped: {procs}")
    pids = rc.games_in_snapshot(procs, snap)
    check(pids == [101, 105, 107], f"games in the snapshot: 101, 105 (case, slashes) and 107 (a subfolder); never 102 (another worktree), 103 (no path), 104 (a sibling folder), 106 (not the game): {pids}")
    check(rc.games_in_snapshot([(102, procs[1][1])], snap) == [], "games in the snapshot: a game outside it alone: none")


def test_pgo_recipe_timeout_kills_tree(root):
    """M7b: a run past its timeout has its whole process tree killed (taskkill /T /F), the child included."""
    rc = load("pgo_recipe")
    out = os.path.join(root, "out")
    os.makedirs(out)  # the recipe's first log line makes it before any run
    pidfile = os.path.join(root, "child.pid")
    child = os.path.join(root, "child.py")
    with open(child, "w", encoding="ascii", newline="\n") as f:
        f.write("import os, sys, time\nopen(sys.argv[1], 'w').write(str(os.getpid()))\ntime.sleep(120)\n")
    parent = os.path.join(root, "parent.py")
    with open(parent, "w", encoding="ascii", newline="\n") as f:
        f.write("import subprocess, sys\nsubprocess.run([sys.executable, '-I', sys.argv[1], sys.argv[2]])\n")
    r = rc.Recipe(rc.argparse.Namespace(out=out, worktree=root, sdk=root, tag_prefix="t"))
    start = time.monotonic()
    code = r.to_log("slow.log", [sys.executable, "-I", parent, child, pidfile], timeout=4)
    took = time.monotonic() - start
    check(code == "timeout" and took < 90, f"to_log past its timeout: 'timeout' soon after: {code!r} after {took:.0f} s")
    log = read(r.log_path).decode("utf-8") if os.path.isfile(r.log_path) else ""
    check("taskkill /T /F" in log, f"recipe.log names the tree kill:\n{log}")
    pid = read(pidfile).decode("ascii").strip() if os.path.isfile(pidfile) else ""
    alive = True
    for _ in range(20):
        t = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/NH"], capture_output=True, text=True, errors="replace", timeout=60)
        alive = bool(pid) and pid in t.stdout.split()
        if not alive:
            break
        time.sleep(0.5)
    check(pid.isdigit() and not alive, f"the child of the timed-out run (pid {pid!r}) is gone")
    if pid.isdigit() and alive:
        subprocess.run(["taskkill", "/PID", pid, "/F"], capture_output=True, timeout=60)


def test_pgo_recipe_configure_off(root):
    """M7c: the recipe ends, after the instrumented build ran, with a DOA4_PGO=off configure; its exit goes into
    recipe.log, before the last line, and a failed one does not touch pgo/."""
    rc = load("pgo_recipe")
    wt = os.path.join(root, "wt")
    write(os.path.join(wt, "pgo", "doa4.profdata"), PROFILE_BYTES)
    write(os.path.join(wt, "pgo", "source.txt"), b"game=x\n")

    def stub(code):
        write(os.path.join(wt, "tools", "np_build.bat"),
              f"@echo off\r\necho stub %1 DOA4_PGO=%DOA4_PGO% DOA4_SDK=%DOA4_SDK%\r\nexit /b {code}\r\n".encode("ascii"))

    def recipe(name, instrumented):
        out = os.path.join(root, name)
        r = rc.Recipe(rc.argparse.Namespace(out=out, worktree=wt, sdk=os.path.join(root, "sdk"), tag_prefix="t"))
        r.instrumented = instrumented
        r.log("recipe start: test")  # as main() does first, which makes the folder
        return r, out

    def log_lines(r):
        return read(r.log_path).decode("utf-8").strip().split("\n") if os.path.isfile(r.log_path) else []

    stub(3)
    r, out = recipe("o1", True)
    r.end(None)
    lines = log_lines(r)
    cfg = read(os.path.join(out, "configure_off.log")).decode("utf-8", "replace") if os.path.isfile(os.path.join(out, "configure_off.log")) else ""
    check("stub configure DOA4_PGO=off" in cfg and "exit 3" in cfg, f"configure off: np_build.bat configure with DOA4_PGO=off, logged with its exit:\n{cfg}")
    check(len(lines) >= 2 and "configure off: exit 3" in lines[-2] and lines[-1].endswith(" recipe done"),
          f"configure off failed: its exit in recipe.log, then 'recipe done': {lines}")
    check(read(os.path.join(wt, "pgo", "doa4.profdata")) == PROFILE_BYTES and read(os.path.join(wt, "pgo", "source.txt")) == b"game=x\n",
          "configure off failed: pgo/ is not undone")
    stub(0)
    r, out = recipe("o2", True)
    r.end("a reason")
    lines = log_lines(r)
    check(len(lines) >= 2 and "configure off: exit 0" in lines[-2] and lines[-1].endswith(" recipe stopped: a reason"),
          f"stopped after the build: configure off, then 'recipe stopped': {lines}")
    r, out = recipe("o3", False)
    r.end("before the build")
    lines = log_lines(r)
    check(not os.path.exists(os.path.join(out, "configure_off.log")) and not any("configure off" in l for l in lines)
          and lines[-1:] and lines[-1].endswith(" recipe stopped: before the build"), f"stopped before the build: no configure: {lines}")


# ---- the Linux job: run_host_tests_posix.py, tests/host/posix_manifest.txt, linux-build.yml -------------------------

RUN_POSIX = os.path.join(TOOLS, "run_host_tests_posix.py")
POSIX_MANIFEST = os.path.join(HOST, "posix_manifest.txt")
LINUX_WORKFLOW = os.path.join(REPO, ".github", "workflows", "linux-build.yml")
BACKSLASH = chr(92)
LEFT_OUT_STARTS = ("windows header: ", "needs the SDK: ", "needs game files: ", "other: ")
UPDATER_REFUSAL_FLAGS = ["-D_GLIBCXX_ASSERTIONS", "-DDOA4_UPDATER_HOST_TEST", "-pthread", "-I", "{SDK}/thirdparty"]


def rel_repo(path):
    """`path` as a repo-relative name with '/', or None when it lies outside the repo or holds an unexpanded variable."""
    if "%" in path or "!" in path:
        return None
    rel = os.path.relpath(os.path.normpath(path), REPO)
    return None if rel.startswith("..") or os.path.isabs(rel) else rel.replace(BACKSLASH, "/")


def bat_tokens(line):
    return re.findall(r'(?:[^\s"]+|"[^"]*")+', line)


def read_runner_bat(path):
    """How run_<name>_test.bat builds its test, by the three shapes the manifest's rules name:
    'a' one literal clang-cl line; 'b' the same line with flags, folders or its source list held in set variables (a
    `for ... do set SRCS=!SRCS! "%%f"` over a folder's *.cpp included); 'c' compile loops, separate object and link
    steps, or a Python step. shape None = the file fits none of them.
    Returns a dict: shape; sources (the .cpp files as repo-relative '/' names; for 'c' the test source its /Fe exe
    names, then every literal and loop-expanded .cpp of its compile lines); outside (.cpp tokens that are not repo
    files); options (the other tokens of the one /Fe line of 'a' and 'b', variables expanded); compile_lines (how many
    command lines call clang-cl); sdk (a line names %REXSDK_DIR%)."""
    import glob
    here = os.path.dirname(os.path.abspath(path)) + os.sep
    text = read(path).decode("utf-8", "replace").replace("\r\n", "\n")
    lines = [l.strip() for l in text.split("\n")]
    lines = [l for l in lines if l and not re.match(r"(?i)(rem\b|::)", l)]
    var = {}

    def expand(s):
        s = s.replace("%~dp0", here)
        s = re.sub(r"!(\w+)!", lambda m: var.get(m.group(1).upper(), m.group(0)), s)
        return re.sub(r"%(\w+)%", lambda m: var.get(m.group(1).upper(), m.group(0)), s)

    def calls_compiler(l):
        return re.search(r"(?<![\w-])clang-cl(?![\w-])", l) is not None

    loops, compile_lines, block = [], [], None
    loop_compiles = python_step = 0
    for l in lines:
        m = re.fullmatch(r'(?i)(?:if not defined \w+ )?set "?(\w+)=(.*?)"?', l)
        if m:
            var[m.group(1).upper()] = expand(m.group(2))
            continue
        m = re.fullmatch(r'(?i)for %%(\w) in \("([^"]*\*[^"]*)"\) do set (\w+)=!(\w+)! "%%(\w)"', l)
        if m and m.group(3).upper() == m.group(4).upper() and m.group(1) == m.group(5):
            var[m.group(3).upper()] = " ".join(f'"{p}"' for p in sorted(glob.glob(expand(m.group(2)))))
            continue
        m = re.fullmatch(r'(?i)for %%(\w) in \(([^()"*]+)\) do (.+)', l)
        if m:
            body = [] if m.group(3) == "(" else [m.group(3)]
            loops.append((m.group(1), m.group(2).split(), body))
            block = body if m.group(3) == "(" else None
            loop_compiles += sum(1 for b in body if calls_compiler(b))
            continue
        if block is not None:
            if l == ")":
                block = None
            else:
                block.append(l)
                loop_compiles += 1 if calls_compiler(l) else 0
            continue
        if calls_compiler(l):
            compile_lines.append(l)
        elif re.match(r"(?i)(py|python|python3)(\.exe)?\s", l):
            python_step += 1
    fe = [l for l in compile_lines if "/Fe" in l]
    link = any(t.rstrip('"').lower().endswith(".obj") and not t.startswith("/Fo")
               for l in compile_lines for t in bat_tokens(l))

    def cpps(l):
        return re.findall(r'"([^"]+\.cpp)"', expand(l))

    shape, raw, options = None, [], []
    if loop_compiles or link or python_step:
        shape = "c"
        exe = re.findall(r'/Fe"%OUT%\\(\w+)\.exe"', text)
        raw = [os.path.join(here, exe[0] + ".cpp")] if exe else []
        for l in compile_lines:
            raw += cpps(l)
        for v, items, body in loops:
            for b in body:
                if calls_compiler(b):
                    for item in items:
                        raw += cpps(b.replace("%%" + v, item))
    elif len(fe) == 1:
        bare = fe[0].replace("%~dp0", "").replace("%OUT%", "")
        shape = "b" if re.search(r"%\w+%|!\w+!", bare) else "a"
        raw = cpps(fe[0])
        options = [t for t in bat_tokens(expand(fe[0]))[1:] if not t.rstrip('"').lower().endswith(".cpp")]
    sources, outside = [], []
    for p in raw:
        rel = rel_repo(p)
        if rel is None:
            outside.append(p)
        elif rel not in sources:
            sources.append(rel)
    return {"shape": shape, "sources": sources, "outside": outside, "options": options,
            "compile_lines": len(compile_lines) + loop_compiles, "sdk": any("%rexsdk_dir%" in l.lower() for l in lines)}


def posix_flags(options):
    """The Linux line's flags for a runner's clang-cl options (the manifest's translation rule): each /D<X> is -D<X>,
    except /D_MSVC_STL_HARDENING=1 -> -D_GLIBCXX_ASSERTIONS and the Windows-only three, which are dropped; an include
    folder inside the repo is `-I <folder>`; /clang:<x> is <x>. Returns a dict: flags; outside (include folders that are
    not in the repo); fp (/fp: options); libs (Windows link libraries, dropped); std (a /std: other than c++latest);
    unknown (anything else)."""
    out = {"flags": [], "outside": [], "fp": [], "libs": [], "std": [], "unknown": []}
    i = 0
    while i < len(options):
        t = options[i]
        i += 1
        if t in ("/nologo", "/EHsc", "/O2", "/std:c++latest") or t.startswith("/Fo") or t.startswith("/Fe"):
            continue
        if t.startswith("/std:"):
            out["std"].append(t)
        elif t == "/D_MSVC_STL_HARDENING=1":
            out["flags"].append("-D_GLIBCXX_ASSERTIONS")
        elif t.startswith("/D_WIN32_WINNT=") or t in ("/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX"):
            continue
        elif t.startswith("/D"):
            out["flags"].append("-D" + t[2:])
        elif t.startswith("/I") or t == "-I":
            folder = t[2:] if t.startswith("/I") else (options[i] if i < len(options) else "")
            i += 0 if t.startswith("/I") else 1
            rel = rel_repo(folder.strip('"'))
            if rel is None:
                out["outside"].append(folder.strip('"'))
            else:
                out["flags"] += ["-I", rel]
        elif t.startswith("/clang:"):
            out["flags"].append(t[len("/clang:"):])
        elif t.startswith("/fp:"):
            out["fp"].append(t)
        elif t.lower().endswith(".lib"):
            out["libs"].append(t)
        else:
            out["unknown"].append(t)
    return out


def include_dirs_of(flags):
    """The repo folders a run line's flags give with -I ({SDK} folders are not in the repo and are left out)."""
    return [flags[i + 1] for i, f in enumerate(flags[:-1]) if f == "-I" and "{SDK}" not in flags[i + 1]]


PICOSHA2 = ("src/doa4_updater.cpp", 16, "picosha2/picosha2.h")
# The runner's WINDOWS_HEADERS, written out here: a name dropped from or added to the constant fails a check below.
WINDOWS_HEADERS_PIN = ("windows.h", "winsock2.h", "ws2tcpip.h", "winhttp.h", "bcrypt.h", "iphlpapi.h", "intrin.h", "process.h",
                       "psapi.h", "shlobj.h", "shellapi.h", "shobjidl.h", "knownfolders.h", "objbase.h", "wincodec.h",
                       "winioctl.h", "wrl/client.h", "mstcpip.h", "wlanapi.h", "io.h", "direct.h", "conio.h")
# C and C++ standard headers that the sources include inside a Windows-only block (they are no Windows headers).
STANDARD_IN_WINDOWS_BLOCKS = ("cwchar",)


def left_out_reason(name, rp):
    """The reason the manifest's rules give run_<name>_test.bat for being left out, or None when they give it a run
    line. In the rules' order: a Windows header its sources reach outside a Windows-only block; a multi-step build;
    the SDK or a folder outside the repo named by the .bat; floating-point build options; a quoted include that is in
    no repo folder (updater_refusal's picosha2 is the one allowed)."""
    bat = read_runner_bat(os.path.join(HOST, f"run_{name}_test.bat"))
    if bat["shape"] is None:
        return "no rule: the .bat fits none of the three shapes"
    pf = posix_flags(bat["options"])
    dirs = include_dirs_of(pf["flags"])
    paths = [os.path.join(REPO, s) for s in bat["sources"]]
    hits = ([h for h in (rp.windows_reach(p, REPO) for p in paths) if h]
            or [h for h in (rp.windows_reach(p, REPO, dirs) for p in paths) if h])
    if hits:
        return "windows header: %s:%d %s" % hits[0]
    if bat["shape"] == "c":
        return f"other: multi-step build ({bat['compile_lines']} compile lines)"
    outside = bat["outside"] + pf["outside"]
    if bat["sdk"] or outside:
        what = (["%REXSDK_DIR%"] if bat["sdk"] else []) + [o if "%" in o else os.path.relpath(o, REPO).replace(BACKSLASH, "/")
                                                           for o in outside]
        return "needs the SDK: " + ", ".join(dict.fromkeys(what))
    if pf["fp"]:
        return f"other: floating-point build options ({' '.join(pf['fp'])})"
    unresolved = [u for p in paths for u in rp.unresolved_includes(p, REPO, dirs)
                  if not (name == "updater_refusal" and u == PICOSHA2)]
    if unresolved:
        return "other: an include that is in no repo folder (%s:%d %s)" % unresolved[0]
    if pf["unknown"]:
        return f"no rule: clang-cl options {pf['unknown']}"
    return None


def real_manifest():
    rp = load("run_host_tests_posix")
    text = read(POSIX_MANIFEST).decode("utf-8") if os.path.isfile(POSIX_MANIFEST) else ""
    return (rp, text) + tuple(rp.parse_manifest(text))


def test_posix_manifest_covers_every_runner(root):
    rp, text, runs, left, errors = real_manifest()
    check(bool(text) and not errors, f"posix manifest: the real file parses with no error: {errors or 'no text'}")
    bats = {f[len("run_"):-len("_test.bat")] for f in os.listdir(HOST) if re.fullmatch(r"run_\w+_test\.bat", f)}
    check(len(bats) >= 30, f"posix manifest: the real runners are found: {len(bats)}")
    names = {n for n, _, _ in runs} | {n for n, _ in left}
    check(not (bats - names), f"posix manifest: every run_<name>_test.bat is on a line; missing: {sorted(bats - names)}")
    check(not (names - bats), f"posix manifest: every line names a run_<name>_test.bat; no runner for: {sorted(names - bats)}")
    by_name = {n: (s, f) for n, s, f in runs}
    check("updater_refusal" in by_name and "cpu_info" in by_name,
          f"posix manifest: updater_refusal and cpu_info are run lines: {sorted(by_name)}")
    check(by_name.get("updater_refusal", (None, None))[1] == UPDATER_REFUSAL_FLAGS,
          f"posix manifest: updater_refusal's flags are {UPDATER_REFUSAL_FLAGS}: {by_name.get('updater_refusal')}")
    missing = [f"{n}: {s}" for n, srcs, _ in runs for s in srcs if not os.path.isfile(os.path.join(REPO, s))]
    check(len(runs) >= 1 and not missing, f"posix manifest: at least one run line ({len(runs)}), every source a file: {missing}")
    odd = [f"{n}: {r}" for n, r in left if not r.startswith(LEFT_OUT_STARTS)]
    check(not odd, f"posix manifest: each left-out reason starts with one of {LEFT_OUT_STARTS}: {odd}")
    attrs = read(os.path.join(REPO, ".gitattributes")).decode("ascii").replace("\r\n", "\n").split("\n")
    want = [p + " text eol=lf" for p in (".github/workflows/linux-build.yml", "tools/run_host_tests_posix.py",
                                         "tests/host/posix_manifest.txt")]
    check(all(w in attrs for w in want), f".gitattributes: the three Linux job files are 'text eol=lf': {attrs}")


def test_posix_manifest_parser(root):
    parse = load("run_host_tests_posix").parse_manifest

    def errors_name(errors, *needles):
        return len(errors) == 1 and all(n in errors[0] for n in needles)

    text = "# a comment\n\na | tests/host/a.cpp src/b.cpp | -DX=1 -I {SDK}/inc\nb | tests/host/b.cpp |\n!c | windows header: src/c.h:3 windows.h\n"
    want = ([("a", ["tests/host/a.cpp", "src/b.cpp"], ["-DX=1", "-I", "{SDK}/inc"]), ("b", ["tests/host/b.cpp"], [])],
            [("c", "windows header: src/c.h:3 windows.h")], [])
    got = parse(text)
    check(tuple(got) == want, f"parser: a comment, an empty line, a run line with flags, one with none, a left-out line: {got}")
    got = parse(text.replace("\n", "\r\n"))
    check(tuple(got) == want, f"parser: CRLF line ends give the same result as LF: {got}")
    runs, left, errors = parse("a | x.cpp |\nb | y.cpp |\na | z.cpp | -DZ\n")
    check(runs == [("a", ["x.cpp"], []), ("b", ["y.cpp"], [])] and left == [] and errors_name(errors, "line 3", "a", "line 1"),
          f"parser: a name on two run lines is an error naming it and both lines, and the first stays: {runs} {errors}")
    runs, left, errors = parse("a | x.cpp |\n!a | other: why\n")
    check(runs == [("a", ["x.cpp"], [])] and left == [] and errors_name(errors, "line 2", "a", "line 1"),
          f"parser: a name on a run line and a left-out line is an error: {runs} {left} {errors}")
    runs, left, errors = parse("!a | other: one\n\n!a | other: two\n")
    check(runs == [] and left == [("a", "other: one")] and errors_name(errors, "line 3", "a", "line 1"),
          f"parser: a name on two left-out lines is an error, and the first stays: {left} {errors}")
    for what, bad in (("a run line with one | only", "a | x.cpp\n"), ("an empty name", " | x.cpp |\n"),
                      ("an empty sources field", "a |  | -DX\n"), ("a left-out line with no reason", "!a\n"),
                      ("a left-out line with an empty reason", "!a |  \n"), ("a name that is not a word", "a-b | x.cpp |\n"),
                      ("a left-out line with no name", "! | other: why\n")):
        runs, left, errors = parse("ok | k.cpp |\n" + bad)
        check(runs == [("ok", ["k.cpp"], [])] and left == [] and errors_name(errors, "line 2"),
              f"parser: {what} is an error on its line and adds nothing: {runs} {left} {errors}")
    got = parse("a | x.cpp | -DX | -DY\n")
    check(tuple(got) == ([("a", ["x.cpp"], ["-DX", "|", "-DY"])], [], []), f"parser: a third | belongs to the flags: {got}")
    got = parse("!a | other: p | q\n")
    check(tuple(got) == ([], [("a", "other: p | q")], []), f"parser: a | inside a reason belongs to the reason: {got}")
    got = parse("")
    check(tuple(got) == ([], [], []), f"parser: an empty text is three empty lists: {got}")


def test_posix_manifest_windows_reach(root):
    rp = load("run_host_tests_posix")
    w = os.path.join(root, "walk")

    def src(rel, *lines):
        write(os.path.join(w, rel), ("\n".join(lines) + "\n").encode("ascii"))
        return os.path.join(w, rel)

    def reach(rel, *lines, dirs=()):
        return rp.windows_reach(src(rel, *lines), w, dirs)

    check("windows.h" in rp.WINDOWS_HEADERS and "winsock2.h" in rp.WINDOWS_HEADERS and "vector" not in rp.WINDOWS_HEADERS
          and "unistd.h" not in rp.WINDOWS_HEADERS, f"walk: WINDOWS_HEADERS names the Windows headers only: {sorted(rp.WINDOWS_HEADERS)}")
    got = reach("u1.cpp", "#include <vector>", "#include <windows.h>")
    check(got == ("u1.cpp", 2, "windows.h"), f"walk: an unguarded Windows header is reached, with its file and line: {got}")
    got = reach("u1b.cpp", "#include <vector>", "  #  include <Windows.h>  // the SDK's spelling")
    check(got == ("u1b.cpp", 2, "Windows.h"), f"walk: the header's name is compared without case, spaces allowed: {got}")
    got = reach("u0.cpp", "#include <vector>", "#include <unistd.h>", "// #include <windows.h>", "/* text", "#include <windows.h>", "*/")
    check(got is None, f"walk: a source with no Windows header outside a comment reaches none: {got}")
    src("a/one.h", '#include "two.h"')
    src("a/two.h", "#pragma once", "#include <string>", "#include <winsock2.h>")
    got = reach("u2.cpp", '#include "a/one.h"')
    check(got == ("a/two.h", 3, "winsock2.h"), f"walk: a Windows header two local includes away is reached where it sits: {got}")
    # where a quoted include is looked for: the including file's folder first, then the repo root, then src/
    src("sub/pick.h", "#include <vector>")
    src("pick.h", "#include <windows.h>")
    got = reach("sub/first.cpp", '#include "pick.h"')
    check(got is None, f"walk: the including file's folder comes before the repo root: {got}")
    src("rooted/x.h", "#include <io.h>")
    got = reach("sub/second.cpp", '#include "rooted/x.h"')
    check(got == ("rooted/x.h", 1, "io.h"), f"walk: an include not beside the file is found from the repo root: {got}")
    src("src/deep/s.h", "#include <winhttp.h>")
    got = reach("sub/third.cpp", '#include "deep/s.h"')
    check(got == ("src/deep/s.h", 1, "winhttp.h"), f"walk: then from src/: {got}")
    src("inc/lib/l.h", "#include <bcrypt.h>")
    got = reach("sub/fourth.cpp", '#include "lib/l.h"', dirs=("inc",))
    check(got == ("inc/lib/l.h", 1, "bcrypt.h"), f"walk: then from the run line's -I folders: {got}")
    check(rp.unresolved_includes(os.path.join(w, "sub/fourth.cpp"), w) == [("sub/fourth.cpp", 1, "lib/l.h")]
          and rp.windows_reach(os.path.join(w, "sub/fourth.cpp"), w) is None,
          "walk: without that folder the include is reported as unresolved (file, line, name), not followed")
    check(rp.unresolved_includes(os.path.join(w, "u2.cpp"), w) == [], "walk: a closure whose includes all resolve reports none unresolved")
    # guards
    got = reach("g1.cpp", "#ifdef _WIN32", "#include <windows.h>", "#endif")
    check(got is None, f"walk: inside #ifdef _WIN32 is guarded: {got}")
    got = reach("g2.cpp", "#if defined(_WIN32)  // Windows", "#include <windows.h>", "#endif")
    check(got is None, f"walk: inside #if defined(_WIN32) is guarded: {got}")
    got = reach("g3.cpp", "#ifndef _WIN32", "#include <unistd.h>", "#else", "#include <windows.h>", "#endif")
    check(got is None, f"walk: the #else of #ifndef _WIN32 is guarded: {got}")
    got = reach("g3b.cpp", "#if !defined(_WIN32)", "#include <unistd.h>", "#else", "#include <windows.h>", "#endif")
    check(got is None, f"walk: the #else of #if !defined(_WIN32) is guarded: {got}")
    got = reach("g3c.cpp", "#ifndef _WIN32", "#include <windows.h>", "#else", "#include <io.h>", "#endif")
    check(got == ("g3c.cpp", 2, "windows.h"), f"walk: the first branch of #ifndef _WIN32 is the non-Windows one: {got}")
    got = reach("g4.cpp", "#ifdef _WIN32", "#include <io.h>", "#else", "#include <windows.h>", "#endif")
    check(got == ("g4.cpp", 4, "windows.h"), f"walk: the #else of #ifdef _WIN32 is the non-Windows branch, unguarded: {got}")
    got = reach("g5.cpp", "#ifndef G5_H", "#define G5_H", "#ifdef _WIN32", "#include <io.h>", "#endif", "#include <windows.h>", "#endif")
    check(got == ("g5.cpp", 6, "windows.h"), f"walk: a guard closed before the include does not guard it: {got}")
    got = reach("g6.cpp", "#if defined(_WIN32) || defined(X)", "#include <windows.h>", "#endif")
    check(got == ("g6.cpp", 2, "windows.h"), f"walk: #if defined(_WIN32) || ... is not a Windows-only block: {got}")
    got = reach("g7.cpp", "#ifdef _WIN32", "#ifndef NOMINMAX", "#define NOMINMAX", "#endif", "#include <windows.h>", "#endif")
    check(got is None, f"walk: an unrelated #ifndef closed inside the _WIN32 block leaves the block open: {got}")
    got = reach("g7b.cpp", "#ifdef _WIN32", "#ifdef X", "#include <io.h>", "#else", "#include <windows.h>", "#endif", "#endif")
    check(got is None, f"walk: an unrelated #ifdef / #else inside the _WIN32 block stays guarded: {got}")
    src("winonly.h", "#include <windows.h>")
    got = reach("g8.cpp", "#ifdef _WIN32", '#include "winonly.h"', "#endif", "#include <vector>")
    check(got is None, f"walk: a quoted include inside a Windows-only block is not followed: {got}")
    got = reach("g8b.cpp", "#ifdef _WIN32", "#else", '#include "winonly.h"', "#endif")
    check(got == ("winonly.h", 1, "windows.h"), f"walk: the same include in the non-Windows branch is followed: {got}")
    got = reach("g9.cpp", "#if defined(__linux__)", "#include <unistd.h>", "#elif defined(_WIN32)", "#include <windows.h>", "#endif")
    check(got is None, f"walk: an #elif defined(_WIN32) branch is guarded: {got}")
    got = reach("g9b.cpp", "#ifdef _WIN32", "#include <io.h>", "#elif defined(__APPLE__)", "#include <windows.h>", "#endif")
    check(got == ("g9b.cpp", 4, "windows.h"), f"walk: an #elif after the _WIN32 branch is not Windows-only: {got}")
    got = reach("g9c.cpp", "#if defined(__linux__)", "#elif defined(_WIN32)", "#include <io.h>", "#else", "#include <windows.h>", "#endif")
    check(got == ("g9c.cpp", 5, "windows.h"), f"walk: the #else after an #elif chain is not Windows-only: {got}")
    got = reach("g10.cpp", "#ifdef _MSC_VER", "#include <intrin.h>", "#endif")
    check(got == ("g10.cpp", 2, "intrin.h"), f"walk: a guard the walk does not know (_MSC_VER) counts as unguarded: {got}")
    # an include cycle ends, and the walk goes on after it
    src("c1.h", '#include "c2.h"')
    src("c2.h", '#include "c1.h"')
    got = reach("cyc.cpp", '#include "c1.h"')
    check(got is None, f"walk: an include cycle ends: {got}")
    got = reach("cyc2.cpp", '#include "c1.h"', "#include <conio.h>")
    check(got == ("cyc2.cpp", 2, "conio.h"), f"walk: a header after an include cycle is still reached: {got}")
    # C++23's #elifdef / #elifndef, and text that only looks like a comment or a directive
    got = reach("e1.cpp", "#ifdef _WIN32", "#include <io.h>", "#elifndef FOO", "#include <windows.h>", "#endif")
    check(got == ("e1.cpp", 4, "windows.h"), f"walk: an #elifndef branch after #ifdef _WIN32 is not Windows-only: {got}")
    got = reach("e2.cpp", "#ifdef _WIN32", "#include <io.h>", "#elifdef __linux__", "#include <windows.h>", "#endif")
    check(got == ("e2.cpp", 4, "windows.h"), f"walk: an #elifdef branch after #ifdef _WIN32 is not Windows-only: {got}")
    got = reach("e3.cpp", "#ifdef __linux__", "#include <unistd.h>", "#elifdef _WIN32", "#include <windows.h>", "#endif")
    check(got is None, f"walk: an #elifdef _WIN32 branch is guarded: {got}")
    got = reach("s1.cpp", 'const char* a = "dir/*";', "#include <windows.h>", 'const char* b = "*/";')
    check(got == ("s1.cpp", 2, "windows.h"), f"walk: a /* inside a string opens no comment: {got}")
    got = reach("s2.cpp", "#ifdef _WIN32", 'const char* a = "dir/*";', "#endif", 'const char* b = "*/";', "#include <windows.h>")
    check(got == ("s2.cpp", 5, "windows.h"), f"walk: a /* inside a string does not hide the #endif after it: {got}")
    got = reach("s3.cpp", "// see a/* for the files", "#include <windows.h>", "int x; /* later */")
    check(got == ("s3.cpp", 2, "windows.h"), f"walk: a /* inside a // comment opens no comment: {got}")
    got = reach("r1.cpp", 'const char* t = R"(', "#ifdef _WIN32", ')";', "#include <windows.h>")
    check(got == ("r1.cpp", 4, "windows.h"), f"walk: a directive inside a raw string is text: {got}")
    got = reach("r2.cpp", 'const char* t = R"x(', "#include <windows.h>", ')" still inside', ')x";', "#include <vector>")
    check(got is None, f"walk: an #include inside a raw string is text: {got}")
    got = reach("r3.cpp", "int k = 1'000;", "#ifdef _WIN32", "#include <windows.h>", "#endif", "char c = '\"';", "#include <io.h>")
    check(got == ("r3.cpp", 6, "io.h"), f"walk: a digit separator and a quote in a character literal open nothing: {got}")
    # local_includes: one file's quoted includes outside a Windows-only block, which the walk follows
    found, missing = rp.local_includes(os.path.join(w, "sub/fourth.cpp"), w)
    check((found, missing) == ([], [(1, "lib/l.h")])
          and rp.local_includes(os.path.join(w, "u2.cpp"), w) == ([(1, "a/one.h", os.path.normpath(os.path.join(w, "a/one.h")))], [])
          and rp.local_includes(os.path.join(w, "g8.cpp"), w) == ([], []),
          f"walk: local_includes gives a file's resolved and unresolved quoted includes, none from a Windows-only block: {found} {missing}")
    # the second net: a call spelled the Microsoft way outside a Windows-only block
    m1 = src("m1.cpp", "#ifdef _WIN32", "#include <process.h>", "#endif", "#include <string>",  # costumes_host_test.cpp as it was
             'static std::string Name() { return "check_" + std::to_string(unsigned(_getpid())) + ".bin"; }')
    got = rp.msvc_calls(m1, w)
    check(got == [("m1.cpp", 5, "_getpid")], f"calls: _getpid() outside the block that includes <process.h> is found: {got}")
    m2 = src("m2.cpp", "#ifdef _WIN32", "int a = _getpid(); errno_t e = fopen_s(&f, n, m);", "#else",
             "int a = getpid(); x._flush(); ns::_init(); p->_go(); int _Big(); int size = sizeof(a_s);", "#endif",
             "// _getpid() in a comment", 'const char* t = "_getpid()"; /* fopen_s(a) */', 'const char* u = R"(', "_getpid()", ')";')
    got = rp.msvc_calls(m2, w)
    check(got == [], f"calls: none in a Windows-only block, a comment, a string, a member or a qualified name: {got}")
    src("mh.h", "inline int Open() { return fopen_s (0, 0, 0); }")
    got = rp.msvc_calls(src("m3.cpp", "#ifdef _WIN32", "#else", '#include "mh.h"', "#endif", "#define PID _getpid()"), w)
    check(got == [("mh.h", 1, "fopen_s"), ("m3.cpp", 5, "_getpid")], f"calls: found through a local include and in a #define: {got}")

    # the real tree
    _, text, runs, left, errors = real_manifest()
    check(len(runs) >= 1 and not errors, f"reach: the real manifest has run lines ({len(runs)}) and no error: {errors}")
    unresolved = set()
    for name, sources, flags in runs:
        dirs = include_dirs_of(flags)
        hits = [h for h in (rp.windows_reach(os.path.join(REPO, s), REPO, dirs) for s in sources) if h]
        check(not hits, f"reach: run line {name} reaches no Windows header outside a _WIN32 block: {hits}")
        calls = [c for s in sources for c in rp.msvc_calls(os.path.join(REPO, s), REPO, dirs)]
        check(not calls, f"calls: run line {name} makes no Microsoft-spelled call outside a _WIN32 block: {calls}")
        for s in sources:
            unresolved.update(rp.unresolved_includes(os.path.join(REPO, s), REPO, dirs))
        bat = read_runner_bat(os.path.join(HOST, f"run_{name}_test.bat"))
        want = posix_flags(bat["options"])["flags"] + (UPDATER_REFUSAL_FLAGS[2:] if name == "updater_refusal" else [])
        check(bat["shape"] in ("a", "b") and sources == bat["sources"] and flags == want and not bat["outside"],
              f"reach: run line {name} names its .bat's sources and translated flags: shape {bat['shape']}, "
              f"{sources} / {bat['sources']}, {flags} / {want}, outside {bat['outside']}")
    check(unresolved == {("src/doa4_updater.cpp", 16, "picosha2/picosha2.h")},
          f"reach: the run lines' one unresolved quoted include is picosha2 (it lives in the SDK): {sorted(unresolved)}")
    for name, reason in left:
        path = os.path.join(HOST, f"run_{name}_test.bat")
        bat = read_runner_bat(path) if os.path.isfile(path) else {"shape": None, "sources": []}
        check(bat["shape"] in ("a", "b", "c"), f"reach: run_{name}_test.bat fits one of the three shapes: {bat['shape']}")
        if reason.startswith("windows header: "):
            m = re.fullmatch(r"windows header: (\S+):(\d+) (\S+)", reason)
            hits = [h for h in (rp.windows_reach(os.path.join(REPO, s), REPO) for s in bat["sources"]) if h]
            check(bool(m) and bool(hits) and hits[0] == (m.group(1), int(m.group(2)), m.group(3)),
                  f"reach: left-out {name} names the walk's first Windows header {hits[:1]}: {reason!r}")
        elif reason.startswith("other: multi-step build"):
            check(bat["shape"] == "c" and reason == f"other: multi-step build ({bat['compile_lines']} compile lines)",
                  f"reach: left-out {name} is a multi-step runner with {bat.get('compile_lines')} compile lines: shape {bat['shape']}, {reason!r}")
    # every line is the one the rules give its runner: a left-out reason is the rules' own text, never an invented one
    for name, reason in left:
        want = left_out_reason(name, rp) if os.path.isfile(os.path.join(HOST, f"run_{name}_test.bat")) else "no runner"
        check(reason == want, f"rules: left-out {name} carries the reason the rules give ({want!r}): {reason!r}")
    wrong = [(name, left_out_reason(name, rp)) for name, _, _ in runs]
    check(bool(runs) and all(r is None for _, r in wrong), f"rules: no run line is a runner the rules leave out: {[w for w in wrong if w[1]]}")
    # WINDOWS_HEADERS: the pinned set; every header found inside a Windows-only block, over all the runners' closures,
    # is in it (or is a standard header); a left-out test that reaches one of them unguarded says so
    check(rp.WINDOWS_HEADERS == frozenset(WINDOWS_HEADERS_PIN), f"headers: WINDOWS_HEADERS is the pinned set: "
          f"{sorted(rp.WINDOWS_HEADERS ^ frozenset(WINDOWS_HEADERS_PIN))}")
    guarded, silent = set(), []
    reasons = dict(left)
    for bat_file in sorted(f for f in os.listdir(HOST) if re.fullmatch(r"run_\w+_test\.bat", f)):
        name = bat_file[len("run_"):-len("_test.bat")]
        bat = read_runner_bat(os.path.join(HOST, bat_file))
        dirs = include_dirs_of(posix_flags(bat["options"])["flags"])
        events = [e for s in bat["sources"] for e in rp.walk(os.path.join(REPO, s), REPO, dirs)]
        guarded.update(h.lower() for kind, _, _, h in events if kind == "guarded")
        open_windows = sorted({h.lower() for kind, _, _, h in events if kind == "angle"} & set(WINDOWS_HEADERS_PIN))
        if open_windows and not reasons.get(name, "").startswith("windows header: "):
            silent.append((name, open_windows))
    odd = guarded - set(WINDOWS_HEADERS_PIN) - set(STANDARD_IN_WINDOWS_BLOCKS)
    check(len(guarded) >= 8 and {"bcrypt.h", "mstcpip.h", "wlanapi.h"} <= guarded and not odd,
          f"headers: every header inside a Windows-only block is a known Windows header (or standard): unplaced {sorted(odd)}, seen {sorted(guarded)}")
    check(not silent, f"headers: a runner that reaches a Windows header unguarded is left out for it: {silent}")


FAKE_CXX = r'''"""A stand-in for the C++ compiler (the release-tools suite's): it checks the line it is given and writes a
"program" (a .bat that starts fakeprog.py on the first source) at the -o path. The first source says what it expects
(// argv: the tokens between -O2 and -o) and what its program does (// print:, // eprint:, // sleep:, // exit:)."""
import json
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
a = sys.argv[1:]
with open(os.path.join(here, "log.jsonl"), "a", encoding="utf-8") as f:
    f.write(json.dumps({"what": "compile", "argv": a, "cwd": os.getcwd(), "pid": os.getpid()}) + "\n")


def fail(why):
    print("fake compiler: " + why)
    sys.exit(9)


if a[:2] != ["-std=c++23", "-O2"] or a.count("-o") != 1 or a.index("-o") != len(a) - 2:
    fail("the line is not '-std=c++23 -O2 ... -o <program>': %r" % (a,))
middle, out = a[2:-2], a[-1]
sources = [t for t in middle if t.endswith(".cpp")]
if not sources or not all(os.path.isfile(s) for s in sources):
    fail("a source is not a file seen from the compiler's folder: %r" % (sources,))
lines = open(sources[0], encoding="utf-8").read().splitlines()
want = [json.loads(l[len("// argv: "):]) for l in lines if l.startswith("// argv: ")]
if len(want) != 1 or middle != want[0]:
    fail("flags and sources differ: got %r, the source says %r" % (middle, want))
if "// compile: fail" in lines:
    print("fixture.cpp:1:1: error: the fake compile failure")
    sys.exit(1)
if "// compile: sleep" in lines:  # a compiler that hangs
    import time
    time.sleep(60)
if "// compile: no program" in lines:  # a compiler that says 0 and writes nothing
    sys.exit(0)
with open(out, "w", encoding="ascii", newline="") as f:
    f.write("@echo off\r\n" + '"%s" -I "%s" "%s"\r\n' % (sys.executable, os.path.join(here, "fakeprog.py"),
                                                       os.path.abspath(sources[0])) + "exit /b %errorlevel%\r\n")
'''

FAKE_PROG = r'''"""The program the fake compiler's .bat starts: does what the fixture source's // lines say."""
import json
import os
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
lines = open(sys.argv[1], encoding="utf-8").read().splitlines()
try:
    given = sys.stdin.read()
except Exception as e:
    given = "stdin: %r" % (e,)
with open(os.path.join(here, "log.jsonl"), "a", encoding="utf-8") as f:
    f.write(json.dumps({"what": "program", "source": os.path.basename(sys.argv[1]), "cwd": os.getcwd(), "pid": os.getpid(),
                        "stdin": given}) + "\n")
code = 0
for l in lines:
    if l.startswith("// print: "):
        print(l[len("// print: "):], flush=True)
    elif l.startswith("// eprint: "):
        print(l[len("// eprint: "):], file=sys.stderr, flush=True)
    elif l.startswith("// sleep: "):
        time.sleep(float(l[len("// sleep: "):]))
    elif l.startswith("// exit: "):
        code = int(l[len("// exit: "):])
sys.exit(code)
'''


def posix_setup(root):
    """(repo, cxx, log): a stand-in repo folder, the fake compiler's .bat (a space in both paths) and its log."""
    tool = os.path.join(root, "fake cxx")
    write(os.path.join(tool, "fakecxx.py"), FAKE_CXX.encode("ascii"))
    write(os.path.join(tool, "fakeprog.py"), FAKE_PROG.encode("ascii"))
    cxx = os.path.join(tool, "fakecxx.bat")
    write(cxx, ("\r\n".join(["@echo off", f'"{sys.executable}" -I "%~dp0fakecxx.py" %*', "exit /b %errorlevel%"]) + "\r\n").encode("ascii"))
    repo = os.path.join(root, "the repo")
    os.makedirs(os.path.join(repo, "tests", "host"))
    return repo, cxx, os.path.join(tool, "log.jsonl")


def posix_source(repo, rel, argv, *lines):
    """A fixture source: `argv` = the words the fake compiler must get between -O2 and -o (a text, or a list when a
    word may hold a space); `lines` = what its program does."""
    import json
    argv = argv.split() if isinstance(argv, str) else list(argv)
    write(os.path.join(repo, rel), ("\n".join(["// argv: " + json.dumps(argv)] + ["// " + l for l in lines]) + "\n").encode("ascii"))


def posix_run(repo, cxx, log, manifest, *args, env=None, where=None):
    """Runs the POSIX runner on `manifest` (a text, written to the stand-in repo's own manifest path, or to `where` and
    given with --manifest) with the fake compiler; (exit code, output, the fake's records)."""
    import json
    path = where or os.path.join(repo, "tests", "host", "posix_manifest.txt")
    write(path, manifest.encode("utf-8"))
    if os.path.exists(log):
        os.remove(log)
    extra = ["--manifest", where] if where else []
    code, text = run(RUN_POSIX, "--repo", repo, "--cxx", cxx, "--exe-suffix", ".bat", *extra, *args, env=env)
    records = [json.loads(l) for l in read(log).decode("utf-8").splitlines()] if os.path.exists(log) else []
    return code, text, records


def last_line(text):
    lines = text.rstrip("\n").split("\n")
    return lines[-1] if lines else ""


def same_folder(a, b):
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def pid_alive(pid):
    p = subprocess.run(["tasklist", "/NH", "/FI", f"PID eq {pid}"], capture_output=True, text=True, errors="replace")
    return re.search(rf"\s{pid}\s", p.stdout) is not None


def test_posix_runner_verdicts(root):
    repo, cxx, log = posix_setup(root)
    passed = "print: PASSED: 3 checks, 0 failures"
    posix_source(repo, "tests/host/pass_host_test.cpp", "-DX=1 -I inc tests/host/pass_host_test.cpp src/extra.cpp", "print: working", passed)
    write(os.path.join(repo, "src", "extra.cpp"), b"// a second source\n")
    posix_source(repo, "tests/host/fail.cpp", "tests/host/fail.cpp", "print: FAILED: 3 checks, 1 failures", "exit: 1")
    posix_source(repo, "tests/host/nosum.cpp", "tests/host/nosum.cpp", "print: hello")
    posix_source(repo, "tests/host/nocomp.cpp", "tests/host/nocomp.cpp", "compile: fail")
    run_pass = "pass | tests/host/pass_host_test.cpp src/extra.cpp | -DX=1 -I inc\n"
    mixed = ("# the stand-in manifest\n" + run_pass + "fail | tests/host/fail.cpp |\nnosum | tests/host/nosum.cpp |\n"
             "nocomp | tests/host/nocomp.cpp |\n!gone | windows header: src/x.h:3 windows.h\n")
    code, text, rec = posix_run(repo, cxx, log, mixed)
    check(code == 1, f"posix runner, mixed manifest: exit 1: {code}\n{text}")
    check(line_for(text, "pass") == "pass: passed 3", f"posix runner: 'pass: passed 3': {line_for(text, 'pass')!r}\n{text}")
    check(line_for(text, "fail") == "fail: FAILED (FAILED: 3 checks, 1 failures; exit 1)", f"posix runner: the FAILED line: {line_for(text, 'fail')!r}\n{text}")
    check(line_for(text, "nosum") == "nosum: did not run (no summary line; exit 0)", f"posix runner: no summary line: {line_for(text, 'nosum')!r}\n{text}")
    check(line_for(text, "nocomp") == "nocomp: did not run (compile failed; exit 1)", f"posix runner: a compile failure: {line_for(text, 'nocomp')!r}\n{text}")
    check("left out: gone (windows header: src/x.h:3 windows.h)" in text.split("\n"), f"posix runner: the left-out line\n{text}")
    check(last_line(text) == "posix host tests: 3 ran, 1 passed, 1 left out, manifest 4", f"posix runner: the last line: {last_line(text)!r}\n{text}")
    after_fail = text.split(line_for(text, "fail") or "\0", 1)[-1].split("\n")
    check("    | FAILED: 3 checks, 1 failures" in after_fail, f"posix runner: a failed test's output follows its line\n{text}")
    after_nocomp = text.split(line_for(text, "nocomp") or "\0", 1)[-1].split("\n")
    check("    | fixture.cpp:1:1: error: the fake compile failure" in after_nocomp, f"posix runner: the compiler's output follows a compile failure\n{text}")
    compiles = [r for r in rec if r["what"] == "compile"]
    programs = [r for r in rec if r["what"] == "program"]
    first = compiles[0]["argv"] if compiles else []
    check(first[:-1] == ["-std=c++23", "-O2", "-DX=1", "-I", "inc", "tests/host/pass_host_test.cpp", "src/extra.cpp", "-o"]
          and os.path.basename(first[-1]) == "pass.bat",
          f"posix runner: the compile line is -std=c++23 -O2, the flags, every source, -o <tmp>/<name><suffix>: {first}")
    check(len(compiles) == 4 and [p["source"] for p in programs] == ["pass_host_test.cpp", "fail.cpp", "nosum.cpp"],
          f"posix runner: four compiles, three programs started, in the manifest's order: {rec}")
    check(bool(compiles) and all(same_folder(c["cwd"], repo) for c in compiles), f"posix runner: the compiler runs in the repo root: {[c['cwd'] for c in compiles]}")
    check(bool(programs) and all(same_folder(p["cwd"], os.path.join(repo, "tests", "host")) for p in programs),
          f"posix runner: a test program runs in tests/host: {[p['cwd'] for p in programs]}")
    check(bool(programs) and all(p["stdin"] == "" for p in programs), f"posix runner: a test program gets no input: {[p['stdin'] for p in programs]}")
    check(bool(first) and not os.path.exists(os.path.dirname(first[-1])), f"posix runner: the run's temporary folder is removed: {first[-1:]}")
    # --only
    code, text, rec = posix_run(repo, cxx, log, mixed, "--only", "pass")
    check(code == 0 and line_for(text, "pass") == "pass: passed 3" and line_for(text, "fail") is None
          and last_line(text) == "posix host tests: 1 ran, 1 passed, 1 left out, manifest 4" and len(rec) == 2
          and "left out: gone (windows header: src/x.h:3 windows.h)" in text.split("\n"),
          f"posix runner --only pass: exit 0, one test, the whole manifest's counts: exit {code}, {len(rec)} records\n{text}")
    code, text, rec = posix_run(repo, cxx, log, mixed, "--only", "pass", "fail")
    check(code == 1 and last_line(text) == "posix host tests: 2 ran, 1 passed, 1 left out, manifest 4",
          f"posix runner --only pass fail: exit 1 (one named test failed): exit {code}\n{text}")
    code, text, rec = posix_run(repo, cxx, log, mixed, "--only", "zz")
    check(code == 1 and "zz" in text and rec == [], f"posix runner --only zz: refused, nothing compiled: exit {code}, {rec}\n{text}")
    code, text, rec = posix_run(repo, cxx, log, mixed, "--only", "pass", "gone")
    check(code == 1 and "gone" in text and rec == [], f"posix runner --only with a left-out name: refused, nothing compiled: exit {code}, {rec}\n{text}")
    # the passing test alone, its manifest given with --manifest
    code, text, rec = posix_run(repo, cxx, log, run_pass, where=os.path.join(root, "elsewhere", "m.txt"))
    check(code == 0 and last_line(text) == "posix host tests: 1 ran, 1 passed, 0 left out, manifest 1",
          f"posix runner, one passing test (--manifest): exit 0: exit {code}\n{text}")
    # ran == manifest alone decides: one pass, one compile failure
    code, text, rec = posix_run(repo, cxx, log, run_pass + "nocomp | tests/host/nocomp.cpp |\n")
    check(code == 1 and last_line(text) == "posix host tests: 1 ran, 1 passed, 0 left out, manifest 2",
          f"posix runner, a pass and a compile failure: exit 1, '1 ran, 1 passed': exit {code}\n{text}")
    code, text, rec = posix_run(repo, cxx, log, "!gone | other: why\n")
    check(code == 1 and last_line(text) == "posix host tests: 0 ran, 0 passed, 1 left out, manifest 0" and rec == [],
          f"posix runner, a manifest with no run line: exit 1: exit {code}\n{text}")
    # judged by the test's own summary shape and by the exit code; stderr counts
    posix_source(repo, "tests/host/speakers.cpp", "tests/host/speakers.cpp", "print: all speakers checks passed")
    posix_source(repo, "tests/host/x3.cpp", "tests/host/x3.cpp", passed, "exit: 3")
    posix_source(repo, "tests/host/err.cpp", "tests/host/err.cpp", "eprint: PASSED: 5 checks, 0 failures")
    code, text, rec = posix_run(repo, cxx, log, "speakers | tests/host/speakers.cpp |\nx3 | tests/host/x3.cpp |\nerr | tests/host/err.cpp |\n")
    check(line_for(text, "speakers") == "speakers: passed", f"posix runner: a test with an older summary shape is judged by its own: {line_for(text, 'speakers')!r}\n{text}")
    check(line_for(text, "x3") == "x3: FAILED (PASSED: 3 checks, 0 failures; exit 3)", f"posix runner: a PASSED line with exit 3 is FAILED: {line_for(text, 'x3')!r}\n{text}")
    check(line_for(text, "err") == "err: passed 5", f"posix runner: a summary on stderr is read: {line_for(text, 'err')!r}\n{text}")
    check(code == 1 and last_line(text) == "posix host tests: 3 ran, 2 passed, 0 left out, manifest 3", f"posix runner, exit 3: exit 1 and the counts: exit {code}\n{text}")
    # {SDK}
    env = {k: v for k, v in os.environ.items() if k.upper() != "REXSDK_DIR"}
    sdk = os.path.join(root, "sdk dir")
    os.makedirs(sdk)
    posix_source(repo, "tests/host/sdk.cpp", ["-I", sdk + "/thirdparty", "tests/host/sdk.cpp"], passed)
    one_sdk = "sdk | tests/host/sdk.cpp | -I {SDK}/thirdparty\n"
    for what, e in (("unset", env), ("not a folder", dict(env, REXSDK_DIR=os.path.join(root, "no such folder")))):
        code, text, rec = posix_run(repo, cxx, log, one_sdk, env=e)
        check(code == 1 and (line_for(text, "sdk") or "").startswith("sdk: did not run (") and "REXSDK_DIR" in (line_for(text, "sdk") or "")
              and last_line(text) == "posix host tests: 0 ran, 0 passed, 0 left out, manifest 1" and rec == [],
              f"posix runner, {{SDK}} with REXSDK_DIR {what}: did not run, nothing compiled: exit {code}, {rec}\n{text}")
    code, text, rec = posix_run(repo, cxx, log, one_sdk, env=dict(env, REXSDK_DIR=sdk))
    got = rec[0]["argv"] if rec else []
    check(got[2:4] == ["-I", sdk + "/thirdparty"], f"posix runner, {{SDK}}: the compiler gets REXSDK_DIR's folder in its place, as one word: {got}")
    # a parser error: exit 1 and nothing compiled, with and without --only
    bad = run_pass + "pass | tests/host/fail.cpp |\n"
    for args in ((), ("--only", "pass")):
        code, text, rec = posix_run(repo, cxx, log, bad, *args)
        check(code == 1 and "line 2" in text and rec == [] and line_for(text, "pass") is None,
              f"posix runner, a manifest error {args}: exit 1, the error printed, nothing compiled: exit {code}, {rec}\n{text}")
    # the output tail is 40 lines
    posix_source(repo, "tests/host/long.cpp", "tests/host/long.cpp", *[f"print: row {i:02d}" for i in range(1, 51)],
                 "print: FAILED: 2 checks, 1 failures", "exit: 1")
    code, text, rec = posix_run(repo, cxx, log, "long | tests/host/long.cpp |\n")
    tail = [l for l in text.split("\n") if l.startswith("    | ")]
    check(len(tail) == 40 and tail[0] == "    | row 12" and tail[-1] == "    | FAILED: 2 checks, 1 failures",
          f"posix runner: the last 40 lines of a failed test's 51: {len(tail)}, {tail[:1]} .. {tail[-1:]}\n{text}")
    # a program that hangs is stopped with its children, and did not run
    posix_source(repo, "tests/host/hang.cpp", "tests/host/hang.cpp", passed, "sleep: 60")
    t0 = time.time()
    code, text, rec = posix_run(repo, cxx, log, "hang | tests/host/hang.cpp |\n", "--timeout", "2")
    took = time.time() - t0
    check(code == 1 and line_for(text, "hang") == "hang: did not run (timed out after 2 s)"
          and last_line(text) == "posix host tests: 0 ran, 0 passed, 0 left out, manifest 1",
          f"posix runner, a hung program: did not run, '0 ran', exit 1: exit {code}\n{text}")
    check(took < 40, f"posix runner, a hung program: stopped at the limit, not after its sleep ({took:.0f} s)")
    pids = [r["pid"] for r in rec if r["what"] == "program"]
    gone = False
    for _ in range(20):
        gone = bool(pids) and not any(pid_alive(p) for p in pids)
        if gone or not pids:
            break
        time.sleep(0.5)
    check(gone, f"posix runner, a hung program: its process (a child of the started .bat) is gone afterwards: pids {pids}")
    # a compiler that hangs is stopped the same way, and the test did not run
    posix_source(repo, "tests/host/slowcc.cpp", "tests/host/slowcc.cpp", "compile: sleep", passed)
    t0 = time.time()
    code, text, rec = posix_run(repo, cxx, log, "slowcc | tests/host/slowcc.cpp |\n", "--compile-timeout", "2")
    took = time.time() - t0
    check(code == 1 and line_for(text, "slowcc") == "slowcc: did not run (compile timed out after 2 s)"
          and last_line(text) == "posix host tests: 0 ran, 0 passed, 0 left out, manifest 1" and took < 40
          and not [r for r in rec if r["what"] == "program"],
          f"posix runner, a hung compiler: did not run, '0 ran', exit 1, stopped at the limit ({took:.0f} s): exit {code}\n{text}")
    pids = [r["pid"] for r in rec if r["what"] == "compile"]
    gone = False
    for _ in range(20):
        gone = bool(pids) and not any(pid_alive(p) for p in pids)
        if gone or not pids:
            break
        time.sleep(0.5)
    check(gone, f"posix runner, a hung compiler: its process (a child of the compiler's .bat) is gone afterwards: pids {pids}")
    code, text, rec = posix_run(repo, cxx, log, "slowcc | tests/host/slowcc.cpp |\n" + run_pass, "--only", "pass", "--compile-timeout", "30")
    check(code == 0 and line_for(text, "pass") == "pass: passed 3", f"posix runner, a compile within its limit is judged as before: exit {code}\n{text}")
    # a compiler that exits 0 and writes no program did not compile the test
    posix_source(repo, "tests/host/noprog.cpp", "tests/host/noprog.cpp", "compile: no program", passed)
    code, text, rec = posix_run(repo, cxx, log, "noprog | tests/host/noprog.cpp |\n")
    check(code == 1 and line_for(text, "noprog") == "noprog: did not run (compile failed; exit 0)"
          and last_line(text) == "posix host tests: 0 ran, 0 passed, 0 left out, manifest 1",
          f"posix runner, a compile that says 0 and writes no program: did not run, '0 ran': exit {code}\n{text}")
    check(load("run_host_tests_posix").DEFAULT_COMPILE_TIMEOUT == 1800, "posix runner: the default compile timeout is 1800 s")
    check(load("run_host_tests_posix").DEFAULT_TIMEOUT == 600 and load("run_host_tests_posix").TAIL == 40,
          "posix runner: the default timeout is 600 s and the tail 40 lines")
    check(not os.path.exists(os.path.join(TOOLS, "__pycache__")), "posix runner: running and loading it leaves no tools/__pycache__")


def test_posix_check_names(root):
    # --check-names: the manifest's names against the run_<name>_test.bat files of tests/host; nothing is compiled
    repo, cxx, log = posix_setup(root)
    host = os.path.join(repo, "tests", "host")
    path = os.path.join(host, "posix_manifest.txt")
    posix_source(repo, "src/a.cpp", "src/a.cpp", "print: PASSED: 1 checks, 0 failures")
    three = "# the stand-in manifest\na | src/a.cpp |\nb | src/a.cpp | -DX=1\n!c | other: a reason\n"

    def runners(*files):
        for f in os.listdir(host):
            if f != "posix_manifest.txt":
                os.remove(os.path.join(host, f))
        for f in files:
            write(os.path.join(host, f), b"@echo off\r\n")

    def names_run(manifest, *args):
        code, text, rec = posix_run(repo, cxx, log, manifest, "--check-names", *args)
        return code, text.rstrip("\n").split("\n"), rec

    def no_line(name):
        return f"run_host_tests_posix: run_{name}_test.bat is on no line of {path}"

    def no_runner(name):
        return f"run_host_tests_posix: {path} names {name}, and tests/host has no run_{name}_test.bat"

    def last(m, r, equal):
        return f"posix manifest names: {m} lines, {r} runners, {'equal' if equal else 'NOT equal'}"

    runners("run_a_test.bat", "run_b_test.bat", "run_c_test.bat")
    code, lines, rec = names_run(three)
    check(code == 0 and lines == [last(3, 3, True)] and rec == [],
          f"check-names, 2 run lines + 1 left-out line and their 3 runners: exit 0, the one line, no compile: {code}, {lines}, {rec}")
    # the recorder's control: the same folder without the option does compile, and prints no names line
    code, text, rec = posix_run(repo, cxx, log, three, "--only", "a")
    check(code == 0 and [r["what"] for r in rec] == ["compile", "program"] and "posix manifest names:" not in text,
          f"check-names control: without the option the fake compiler is called and no names line is printed: {code}, {rec}\n{text}")
    runners("run_a_test.bat", "run_b_test.bat", "run_c_test.bat", "run_d_test.bat")
    code, lines, rec = names_run(three)
    check(code == 1 and lines == [no_line("d"), last(3, 4, False)] and rec == [],
          f"check-names, a runner on no line: exit 1, its line, NOT equal: {code}, {lines}, {rec}")
    runners("run_a_test.bat", "run_c_test.bat")
    code, lines, rec = names_run(three)
    check(code == 1 and lines == [no_runner("b"), last(3, 2, False)] and rec == [],
          f"check-names, a run line with no runner: exit 1, its line, NOT equal: {code}, {lines}, {rec}")
    runners("run_a_test.bat", "run_b_test.bat")
    code, lines, rec = names_run(three)
    check(code == 1 and lines == [no_runner("c"), last(3, 2, False)] and rec == [],
          f"check-names, a left-out line with no runner: exit 1, its line, NOT equal: {code}, {lines}, {rec}")
    # both at once, with as many lines as runners: runner lines first, each list sorted by name
    runners("run_e_test.bat", "run_a_test.bat", "run_d_test.bat", "run_b_test.bat")
    code, lines, rec = names_run(three + "z | src/a.cpp |\n")
    check(code == 1 and lines == [no_line("d"), no_line("e"), no_runner("c"), no_runner("z"), last(4, 4, False)] and rec == [],
          f"check-names, 4 lines and 4 runners that differ: exit 1, runner lines then manifest lines, sorted: {code}, {lines}, {rec}")
    runners()
    code, lines, rec = names_run(three)
    check(code == 1 and lines == [f"run_host_tests_posix: no run_<name>_test.bat in {host}"] and rec == [],
          f"check-names, no runner in the folder: exit 1 and the one line, never 'equal': {code}, {lines}, {rec}")
    # names that are not a runner's: a longer name, no middle part, a prefix
    others = ("run_x_test.bat.bak", "run_test.bat", "xrun_a_test.bat")
    runners(*others)
    code, lines, rec = names_run(three)
    check(code == 1 and lines == [f"run_host_tests_posix: no run_<name>_test.bat in {host}"] and rec == [],
          f"check-names, only files that are not runners: exit 1 and the no-runner line: {code}, {lines}, {rec}")
    runners("run_a_test.bat", "run_b_test.bat", "run_c_test.bat", *others)
    code, lines, rec = names_run(three)
    check(code == 0 and lines == [last(3, 3, True)] and rec == [],
          f"check-names, {others} beside the 3 runners do not count: exit 0, the one line: {code}, {lines}, {rec}")
    code, lines, rec = names_run("a | src/a.cpp\nb | src/a.cpp |\n!c | other: a reason\n")
    check(code == 1 and lines == [f"run_host_tests_posix: {path}: line 1: a run line is '<name> | <sources> | <flags>' (both | are there): 'a | src/a.cpp'"]
          and rec == [],
          f"check-names, a manifest error: exit 1, the parser's line and no names line: {code}, {lines}, {rec}")
    code, lines, rec = names_run(three, "--only", "a")
    check(code == 1 and lines == ["run_host_tests_posix: --check-names takes no --only"] and rec == [],
          f"check-names with --only: refused with one line, exit 1, no compile: {code}, {lines}, {rec}")
    # letter case: the fixed parts of a runner's file name match in any case, as in tools/run_host_tests.py; the name in
    # the middle keeps the file's spelling and equals a manifest name only letter for letter
    four = three + "d | src/a.cpp |\n"
    runners("run_a_test.bat", "run_b_test.bat", "run_c_test.bat", "Run_d_TEST.Bat")
    code, lines, rec = names_run(three)
    check(code == 1 and lines == [f"run_host_tests_posix: Run_d_TEST.Bat is on no line of {path}", last(3, 4, False)] and rec == [],
          f"check-names, Run_d_TEST.Bat on no line: exit 1, a line with the file's own name, NOT equal: {code}, {lines}, {rec}")
    code, lines, rec = names_run(four)
    check(code == 0 and lines == [last(4, 4, True)] and rec == [],
          f"check-names, Run_d_TEST.Bat and a line d: the name is d, exit 0, equal: {code}, {lines}, {rec}")
    runners("run_a_test.bat", "run_b_test.bat", "run_c_test.bat", "run_D_test.bat")
    code, lines, rec = names_run(four)
    check(code == 1 and lines == [f"run_host_tests_posix: run_D_test.bat is on no line of {path}", no_runner("d"), last(4, 4, False)] and rec == [],
          f"check-names, run_D_test.bat and a line d: two lines, NOT equal: {code}, {lines}, {rec}")
    # a repository with no tests/host folder (the manifest given from elsewhere) has no runner: refused, never exit 0
    bare = os.path.join(root, "bare repo")
    os.makedirs(bare)
    code, text = run(RUN_POSIX, "--check-names", "--repo", bare, "--manifest", path)
    check(code == 1 and text == f"run_host_tests_posix: no run_<name>_test.bat in {os.path.join(bare, 'tests', 'host')}\n",
          f"check-names, no tests/host folder: exit 1, the no-runner line and no names line: {code}\n{text}")
    # the real tree: the numbers are this suite's own counts of the real runners and of the real manifest's lines
    rp, text, runs, left, errors = real_manifest()
    bats = [f for f in os.listdir(HOST) if re.fullmatch(r"run_\w+_test\.bat", f)]
    code, text = run(RUN_POSIX, "--check-names")
    check(len(bats) >= 30 and code == 0 and text == last(len(runs) + len(left), len(bats), True) + "\n",
          f"check-names on the real tree: exit 0 and '{last(len(runs) + len(left), len(bats), True)}' alone: {code}\n{text}")


def workflow_steps(text):
    """The steps of a workflow text (each '      - ' item after a '    steps:' line), as texts."""
    m = re.search(r"(?m)^    steps:\n", text)
    return [s for s in re.split(r"(?m)^      - ", text[m.end():]) if s.strip()] if m else []


def upload_paths(step):
    """The path entries of one step: the lines under 'path: |', or the single value; None when it has no path key."""
    lines = step.split("\n")
    for i, l in enumerate(lines):
        m = re.match(r"(\s*)path:\s*(.*)$", l)
        if not m:
            continue
        if m.group(2).strip() not in ("|", "|-", "|+", ">", ">-"):
            return [m.group(2).strip().strip("'\"")]
        out = []
        for k in lines[i + 1:]:
            if k.strip() and len(k) - len(k.lstrip()) <= len(m.group(1)):
                break
            if k.strip():
                out.append(k.strip())
        return out
    return None


def upload_path_problems(text):
    """What is wrong with the upload steps of a workflow text: each actions/upload-artifact step must name its files,
    every one a .log file by its exact path (no folder, no *, nothing of the build's output or programs)."""
    problems = []
    for step in workflow_steps(text):
        if "actions/upload-artifact" not in step:
            continue
        paths = upload_paths(step)
        if not paths:
            problems.append("an upload step names no path")
            continue
        for p in paths:
            if not p.endswith(".log"):
                problems.append(f"{p}: not a .log file")
            elif "*" in p or "out/" in p or "bin" in p or p.endswith("/.log"):
                problems.append(f"{p}: a pattern or a build output path")
    return problems


def workflow_fixture(*paths):
    block = "path: " + paths[0] if len(paths) == 1 else "path: |\n" + "\n".join("            " + p for p in paths)
    return ("name: x\njobs:\n  linux:\n    steps:\n      - uses: actions/checkout@v5\n        with: { path: doa4-recomp }\n"
            "      - name: Upload logs\n        if: always()\n        uses: actions/upload-artifact@v4\n        with:\n"
            "          name: logs\n          " + block + "\n")


def test_linux_workflow(root):
    raw = read(LINUX_WORKFLOW) if os.path.isfile(LINUX_WORKFLOW) else b""
    t = raw.decode("utf-8")
    steps = workflow_steps(t)
    check(len(steps) >= 6, f"linux workflow: the file is there and its steps are found: {len(steps)}")
    check(b"\r" not in raw and b"\t" not in raw and bool(raw), "linux workflow: no CR byte and no tab")
    on = re.search(r"(?ms)^on:\n(.*?)^(?=\S)", t)
    on = on.group(1) if on else ""
    check(re.search(r"(?m)^  push:\n    branches: \[main, linux-native\]$", on) is not None and re.search(r"(?m)^  workflow_dispatch:", on) is not None,
          f"linux workflow: runs on a push to main and linux-native and on workflow_dispatch: {on!r}")
    check(bool(t) and re.search(r"(?m)^\s*(paths|paths-ignore|tags|tags-ignore)\s*:", t) is None, "linux workflow: no paths: key and no tags: key")
    check(re.search(r"(?m)^permissions:\n  contents: read\n(?=\S)", t) is not None, "linux workflow: permissions is contents: read and nothing else")
    check(bool(t) and re.search(r":\s*write\b", t) is None, "linux workflow: no line gives a write permission")
    check(re.findall(r"(?m)^\s*runs-on:\s*(.*)$", t) == ["ubuntu-24.04"], f"linux workflow: runs-on ubuntu-24.04: {re.findall(r'(?m)^ *runs-on:.*$', t)}")
    check(re.search(r"(?m)^    timeout-minutes: \d+\s*(#.*)?$", t) is not None, "linux workflow: the job has a timeout-minutes")
    group = re.search(r"(?m)^concurrency:\n(?:  .*\n)*?  group:\s*(\S.*)$", t)
    check(group is not None and "netplay-release" not in group.group(1),
          f"linux workflow: a concurrency group of its own (never the release's): {group.group(1) if group else None!r}")
    checkouts = [s for s in steps if "actions/checkout@" in s]
    check(len(checkouts) == 1 and "actions/checkout@v5" in checkouts[0] and re.search(r"persist-credentials:\s*false\b", checkouts[0]) is not None,
          f"linux workflow: one checkout (v5) with persist-credentials: false: {checkouts}")
    # the "Manifest names" step calls the same runner and has its own checks below
    named = [s for s in steps if s.split("\n")[0].strip() == "name: Manifest names"]
    tests = [s for s in steps if "run_host_tests_posix.py" in s and s not in named]
    calls = [l.strip() for s in tests for l in s.split("\n") if "run_host_tests_posix.py" in l and not l.strip().startswith("#")]
    call = calls[0] if len(calls) == 1 else ""
    check(len(tests) == 1 and "python3 -I tools/run_host_tests_posix.py --cxx clang++-23" in call
          and not any(o in call for o in ("--exe-suffix", "--only", "--manifest", "--repo", "--compile-timeout", "--timeout")),
          f"linux workflow: one step calls 'python3 -I tools/run_host_tests_posix.py --cxx clang++-23' and no test-only option: {calls}")
    check(re.search(r"(?<!\w)DOA4_CI=1(?!\w)", call) is not None or (bool(tests) and re.search(r"(?m)^\s*DOA4_CI:\s*['\"]?1['\"]?\s*$", tests[0]) is not None),
          f"linux workflow: DOA4_CI is 1 for the test step: {call!r}")
    check("| tee host_tests.log" in call and bool(tests) and "set -o pipefail" in tests[0].split(call)[0],
          f"linux workflow: the test step's result survives the tee (set -o pipefail before it): {call!r}")
    name_calls = [l.strip() for s in named for l in s.split("\n") if "run_host_tests_posix.py" in l and not l.strip().startswith("#")]
    check(len(named) == 1 and name_calls == ["run: python3 -I tools/run_host_tests_posix.py --check-names"]
          and re.search(r"(?m)^        working-directory: doa4-recomp$", named[0]) is not None
          and not any(w in named[0] for w in ("REXSDK_DIR", "tee ", "--cxx", "pipefail")),
          f"linux workflow: one step 'Manifest names' in doa4-recomp whose run line is "
          f"'python3 -I tools/run_host_tests_posix.py --check-names' and nothing more: {name_calls}, {named}")
    check(len(named) == 1 and "continue-on-error" not in named[0] and re.search(r"(?m)^\s*if:", named[0]) is None,
          f"linux workflow: the 'Manifest names' step can fail the job: no continue-on-error and no if: key: {named}")
    order = ["checkout" if "actions/checkout@" in s else "names" if s in named else "other" for s in steps]
    check(order[:2] == ["checkout", "names"] and "checkout" not in order[1:] and "names" not in order[2:],
          f"linux workflow: 'Manifest names' is the step after the checkout and before every other step: {order}")
    check(len([s for s in steps if "run_host_tests_posix.py" in s]) == 2 and bool(call) and "--check-names" not in call,
          f"linux workflow: two steps call the runner, and the host tests' call has no --check-names: {call!r}")
    builds = [s for s in steps if "cmake --build --preset linux-amd64-release" in s]
    conf = [l.strip() for s in steps for l in s.split("\n") if "cmake --preset linux-amd64-release" in l and not l.strip().startswith("#")]
    check(len(conf) == 1 and "-DCMAKE_C_COMPILER=clang-23" in conf[0].split() and "-DCMAKE_CXX_COMPILER=clang++-23" in conf[0].split(),
          f"linux workflow: cmake --preset linux-amd64-release with clang-23 and clang++-23: {conf}")
    check(len(builds) == 1 and "| tee build.log" in builds[0] and "set -o pipefail" in builds[0].split("| tee build.log")[0],
          "linux workflow: one build step, its output to build.log with set -o pipefail before it")
    check(any("objects built:" in s for s in steps), "linux workflow: a step prints 'objects built:'")
    check(len(conf) == 1 and '"-DCMAKE_C_FLAGS=-march=x86-64-v2"' in conf[0].split() and '"-DCMAKE_CXX_FLAGS=-march=x86-64-v2"' in conf[0].split(),
          f"linux workflow: the configure passes -march=x86-64-v2 for C and C++, as every other build of this code: {conf}")
    check(len(conf) == 1 and any(w.startswith("-DREXSDK_DIR=") and len(w) > len("-DREXSDK_DIR=") for w in conf[0].split()),
          f"linux workflow: the configure names the SDK folder with -DREXSDK_DIR=: {conf}")
    counts = [s for s in steps if "objects built:" in s]
    check(len(counts) == 1 and re.search(r'(?m)^\s*if \[ "\$n" -eq 0 \]; then exit 1; fi$', counts[0]) is not None
          and re.search(r"(?m)^\s*n=\$\(find out/build/linux-amd64-release -name '\*\.o' \| wc -l\)$", counts[0]) is not None,
          "linux workflow: the object count step counts the build's .o files and fails on zero")
    # the "Runtime libraries" step: the game binary resolves its shared libraries (a fresh build once stopped at start-up
    # with "librexruntime.so: cannot open shared object file", the binary's RUNPATH being $ORIGIN), checked by ldd
    rt = [s for s in steps if s.split("\n")[0].strip() == "name: Runtime libraries"]
    rts = rt[0] if len(rt) == 1 else ""
    rti = steps.index(rts) if rts else -1
    check(len(rt) == 1, f"linux workflow: exactly one step 'Runtime libraries': {len(rt)}")
    obj_i = [i for i, s in enumerate(steps) if "objects built:" in s]
    host_i = [i for i, s in enumerate(steps) if "run_host_tests_posix.py --cxx" in s]
    check(rti > 0 and obj_i == [rti - 1] and len(host_i) == 1 and host_i[0] > rti,
          f"linux workflow: 'Runtime libraries' is the step directly after 'Objects built' and before the host tests: "
          f"runtime {rti}, objects {obj_i}, host tests {host_i}")
    check(bool(rts) and re.search(r"(?m)^        working-directory: doa4-recomp$", rts) is not None,
          "linux workflow: the 'Runtime libraries' step runs in doa4-recomp")
    check(bool(rts) and "continue-on-error" not in rts and re.search(r"(?m)^\s*if:", rts) is None,
          f"linux workflow: the 'Runtime libraries' step can fail the job: no continue-on-error and no if: key: {rt}")
    rm = re.search(r"(?m)^        run: \|\n", rts)
    script = [l.strip() for l in rts[rm.end():].split("\n")] if rm else []
    script = [l for l in script if l and not l.startswith("#")]
    def at(pattern):
        """The index of the one script line that matches pattern in full, else -1 (none, or more than one)."""
        hits = [i for i, l in enumerate(script) if re.fullmatch(pattern, l)]
        return hits[0] if len(hits) == 1 else -1
    check(script[:1] in (["set -o pipefail"], ["set -euo pipefail"]),
          f"linux workflow: the 'Runtime libraries' script starts with set -o pipefail: {script[:1]}")
    find_at = at(r"mapfile -t bins < <\(find out/build/linux-amd64-release -type f -name dead_or_alive_4\)")
    n_at = at(r'n=\$\{#bins\[@\]\}')
    one_at = at(r'if \[ "\$n" -ne 1 \]; then (.*; )?exit 1; fi')
    bin_at = at(r'bin=\$\{bins\[0\]\}')
    check(0 < find_at < n_at < one_at < bin_at,
          f"linux workflow: the 'Runtime libraries' step finds the game binary dead_or_alive_4 under the build folder and "
          f"fails unless exactly one is found: lines {find_at}, {n_at}, {one_at}, {bin_at}")
    ldd_at = at(r'libs=\$\(ldd "\$bin" 2>&1\) \|\| \{ echo "\$libs"; (.*; )?exit 1; \}')
    print_at = at(r'echo "\$libs"')
    nf_at = at(r"if grep -q 'not found' <<< \"\$libs\"; then (.*; )?exit 1; fi")
    check(0 < bin_at < ldd_at < print_at < nf_at,
          f"linux workflow: the 'Runtime libraries' step runs ldd on the binary, prints its output and fails on a "
          f"'not found' line: lines {ldd_at}, {print_at}, {nf_at}")
    dir_at = at(r'if \[ ! -e "\$\(dirname "\$bin"\)/librexruntime\.so" \]; then (.*; )?exit 1; fi')
    check(0 < bin_at < dir_at, f"linux workflow: the 'Runtime libraries' step fails when librexruntime.so is not in the "
          f"binary's own folder: line {dir_at}")
    check(bool(script) and not any(re.search(r"\|\|\s*true\b|\bexit 0\b|^set \+e", l) for l in script),
          f"linux workflow: the 'Runtime libraries' script swallows no failure (no '|| true', 'exit 0' or 'set +e'): {script}")
    # an allowlist over the whole script, so the checks above pin what the step does, not only that its lines exist:
    # every line is one of these, each exactly once, or an echo line of one shape: echo "<text>", where the text holds
    # no quote, back quote or backslash and '$' only as $name or ${name} (an allowed shape, not a ban: printf -v,
    # $[...] and ${a[i=1]} assign without any banned character)
    rt_pinned = (("set -o pipefail", "set -euo pipefail"),
                 ("mapfile -t bins < <(find out/build/linux-amd64-release -type f -name dead_or_alive_4)",),
                 ('n=${#bins[@]}',),
                 (r'''printf '%s\n' "${bins[@]}"''',),
                 ('if [ "$n" -ne 1 ]; then echo "want exactly one game binary"; exit 1; fi',),
                 ('bin=${bins[0]}',),
                 ('libs=$(ldd "$bin" 2>&1) || { echo "$libs"; echo "ldd failed on $bin"; exit 1; }',),
                 ('echo "$libs"',),
                 ("if grep -q 'not found' <<< \"$libs\"; then echo \"a shared library of $bin is not found\"; exit 1; fi",),
                 ('if [ ! -e "$(dirname "$bin")/librexruntime.so" ]; then echo "librexruntime.so is not in $(dirname "$bin")"; '
                  'exit 1; fi',))
    rt_seen = [sum(1 for l in script if l in alts) for alts in rt_pinned]
    rt_free = [l for l in script if not any(l in alts for alts in rt_pinned)]
    def rt_echo(l):
        return re.fullmatch(r'echo "(?:[^"`\\$]|\$[A-Za-z_]\w*|\$\{[A-Za-z_]\w*\})*"', l) is not None
    rt_bad = [l for l in rt_free if not rt_echo(l)]
    check(bool(script) and rt_seen == [1] * len(rt_pinned) and rt_bad == [],
          f"linux workflow: the 'Runtime libraries' script is its {len(rt_pinned)} pinned lines, each once, and lines "
          f'echo "<text>" with $ only as $name or ${{name}}: counts {rt_seen}, other lines {rt_bad}')
    # the exit form reads commands, not echo text
    rt_exits = re.findall(r"\bexit\b[^;}\n]*", "\n".join(l for l in script if not rt_echo(l)))
    check(bool(rt_exits) and all(e.strip() == "exit 1" for e in rt_exits),
          f"linux workflow: the only exit in the 'Runtime libraries' script is 'exit 1': {rt_exits}")
    rt_keys = (["name"] if rts.startswith("name:") else []) + re.findall(r"""(?m)^        ["']?([A-Za-z_][\w-]*)["']?\s*:""", rts)
    check(rt_keys == ["name", "working-directory", "run"],
          f"linux workflow: the 'Runtime libraries' step has the keys name, working-directory and run and no other: {rt_keys}")
    check(bool(t) and "LD_LIBRARY_PATH" not in t and "LD_PRELOAD" not in t,
          "linux workflow: no LD_LIBRARY_PATH and no LD_PRELOAD anywhere (no env: key, no run: line): the game binary "
          "finds its libraries by itself")
    pins = [s for s in steps if "netplay_sdk.txt" in s]
    check(len(pins) == 1 and re.search(r'(?m)^\s*if \[ -z "\$repo" \] \|\| \[ -z "\$commit" \]; then .*exit 1; fi$', pins[0]) is not None
          and pins[0].index("exit 1; fi") < pins[0].index("git clone"),
          "linux workflow: the SDK step stops on an empty repo or commit before it clones")
    installs = [l.strip() for s in steps for l in s.split("\n") if l.strip().startswith("sudo apt-get install ")]
    check(any("libstdc++-14-dev" in l.split() for l in installs), f"linux workflow: libstdc++-14-dev is installed (std::print): {installs}")
    uploads = [s for s in steps if "actions/upload-artifact" in s]
    check(len(uploads) == 1 and re.search(r"(?m)^\s*if:\s*always\(\)\s*$", uploads[0]) is not None and upload_path_problems(t) == [],
          f"linux workflow: one upload step, if: always(), .log files by exact path only: {upload_path_problems(t)}, {uploads}")
    check(bool(t) and not any(w in t for w in ("gh release", "upload-release", "contents: write")), "linux workflow: nothing publishes a release")
    # the upload rule, on stand-in workflow texts
    good = workflow_fixture("doa4-recomp/build.log", "doa4-recomp/host_tests.log")
    check(len(workflow_steps(good)) == 2 and upload_path_problems(good) == [], f"upload rule: two .log files by exact path pass: {upload_path_problems(good)}")
    check(upload_path_problems(workflow_fixture("doa4-recomp/build.log")) == [], "upload rule: a single .log value passes")
    for what, paths in (("the build folder", ["out/build/linux-amd64-release/**"]), ("a program beside *.log", ["*.log", "dead_or_alive_4"]),
                        ("a folder alone", ["doa4-recomp"]), ("a folder with a slash", ["doa4-recomp/"]),
                        ("a .log pattern", ["doa4-recomp/*.log"]), ("a log under the build folder", ["doa4-recomp/out/build/x.log"]),
                        ("a log under a bin folder", ["doa4-recomp/bin/x.log"])):
        check(upload_path_problems(workflow_fixture(*paths)) != [], f"upload rule: {what} is refused: {paths}")
    check(upload_path_problems(good.replace("          path: |\n            doa4-recomp/build.log\n            doa4-recomp/host_tests.log\n", "")) != [],
          "upload rule: an upload step with no path is refused")
    # the release workflow is still the one the older checks read, and the runner's manifest is the committed one
    check(os.path.basename(WORKFLOW) == "netplay-release.yml" and os.path.isfile(WORKFLOW) and "netplay-release" in workflow_text()
          and os.path.normcase(WORKFLOW) != os.path.normcase(LINUX_WORKFLOW), "WORKFLOW still names netplay-release.yml")
    rp = load("run_host_tests_posix")
    check(rp.MANIFEST == "tests/host/posix_manifest.txt" and same_folder(os.path.join(REPO, rp.MANIFEST), POSIX_MANIFEST),
          f"posix runner: its default manifest is tests/host/posix_manifest.txt: {rp.MANIFEST!r}")


def main():
    tests = [(n, f) for n, f in sorted(globals().items()) if n.startswith("test_") and callable(f)]
    for name, fn in tests:
        root = tempfile.mkdtemp(prefix="release_tools_")
        try:
            fn(root)
        except Exception:
            global failures
            failures += 1
            print("FAIL", name, "raised:")
            traceback.print_exc()
        finally:
            shutil.rmtree(root, ignore_errors=True)
    print(f"{checks} checks, {failures} failures")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
