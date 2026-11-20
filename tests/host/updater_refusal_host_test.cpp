// Host test for the updater's platform rule (native Linux bring-in, Task 7): a non-Windows build never downloads or
// installs a release (the release's files are Windows programs), whatever it reports itself as; it may still list
// releases (CheckOnly). Portable: standard C++ and src/doa4_updater.h only, so the same file runs on Windows
// (run_updater_refusal_test.bat) and on Linux (the Linux job). The rule's value comes in through Env::may_install and
// CleanUpAtStart's second argument; each refusal is checked by what it leaves (nothing asked, the folder unchanged, no
// offer, no latch), and a control with may_install = true installs through the same fake, so the test tells the two
// rules apart. No network: the HTTP source is a fake; GameEnv() is called only to read its may_install.
// Files go under <temp>/doa4_updater_refusal_<time stamp>/ and only that folder is removed at the end.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "../../src/doa4_updater.h"

namespace fs = std::filesystem;
namespace up = doa4::updater;

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    ++g_checks;                                                   \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)
#define CHECK_EQ(a, b)                                                                                    \
  do {                                                                                                    \
    ++g_checks;                                                                                           \
    const std::string va_ = (a), vb_ = (b);                                                               \
    if (va_ != vb_) {                                                                                     \
      std::printf("FAIL %s:%d: %s\n   got:  [%s]\n   want: [%s]\n", __FILE__, __LINE__, #a, va_.c_str(), \
                  vb_.c_str());                                                                           \
      ++g_failures;                                                                                       \
    }                                                                                                     \
  } while (0)

static const char* FoundName(up::Found f) {
  return f == up::Found::kNewer ? "newer" : f == up::Found::kUpToDate ? "up to date" : "failed";
}
#define CHECK_FOUND(r, want) CHECK_EQ(FoundName((r).found), FoundName(want))

// ---- files ----
static fs::path g_root;
static int g_dir_n = 0;
static fs::path NewDir(const std::string& tag) {
  const fs::path d = g_root / (tag + "_" + std::to_string(++g_dir_n));
  std::error_code ec;
  fs::create_directories(d, ec);
  return d;
}
static void Write(const fs::path& p, const std::string& bytes) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(bytes.data(), std::streamsize(bytes.size()));
}
static bool Read(const fs::path& p, std::string* out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}
static bool Exists(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}
static bool Holds(const fs::path& p, const std::string& want) {
  std::string got;
  return Read(p, &got) && got == want;
}
// Every file and folder under `dir` (relative generic path -> bytes, "<dir>" for a folder, "<unreadable>" when a file
// cannot be read). Never throws.
static std::map<std::string, std::string> Snapshot(const fs::path& dir) {
  std::map<std::string, std::string> m;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    const std::string rel = fs::relative(it->path(), dir, e2).generic_string();
    if (it->is_directory(e2)) {
      m[rel] = "<dir>";
    } else {
      std::string bytes;
      m[rel] = Read(it->path(), &bytes) ? bytes : std::string("<unreadable>");
    }
  }
  if (ec) m["<iteration error>"] = ec.message();
  return m;
}
static std::string Describe(const std::map<std::string, std::string>& m) {
  std::string s;
  for (const auto& kv : m) s += kv.first + "(" + std::to_string(kv.second.size()) + ") ";
  return s;
}

// ---- the fixture: a release whose three game files hold "abc" ----
static const char* const kShaAbc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";  // FIPS 180-2
static const std::string kOld[3] = {"old exe bytes", "old runtime bytes", "old tracy bytes"};
static std::string Asset(const std::string& name, const std::string& tag) {
  return "{\"name\":\"" + name + "\",\"browser_download_url\":\"https://github.com/o/r/releases/download/" + tag + "/" +
         name + "\"}";
}
static std::string Release(const std::string& tag, const std::vector<std::string>& names) {
  std::string a;
  for (size_t i = 0; i < names.size(); ++i) a += (i ? "," : "") + Asset(names[i], tag);
  return "{\"tag_name\":\"" + tag + "\",\"draft\":false,\"prerelease\":true,\"assets\":[" + a + "]}";
}
static const std::vector<std::string> kAll = {"dead_or_alive_4.exe", "rexruntime.dll", "TracyClient.dll",
                                              "SHA256SUMS.txt"};

struct FakeHttp {
  std::map<std::string, std::string> bodies;
  std::vector<std::string> asked;
  bool Get(const std::string& url, bool, const up::Sink& sink, std::string* error) {
    asked.push_back(url);
    const auto it = bodies.find(url);
    if (it == bodies.end()) {
      *error = "HTTP 404";
      return false;
    }
    const std::string& b = it->second;
    for (size_t at = 0; at < b.size(); at += 4096) {
      if (!sink(b.data() + at, std::min<size_t>(4096, b.size() - at))) {
        *error = "stopped by the sink";
        return false;
      }
    }
    return true;
  }
};
static const std::string kApi = "https://api.github.com/repos/o/r/releases?per_page=30";
static const std::string kTag = "netplay-v1.0.6";
static std::string Url(const std::string& name) { return "https://github.com/o/r/releases/download/" + kTag + "/" + name; }
static void Serve(FakeHttp& h) {
  h.bodies[kApi] = "[" + Release(kTag, kAll) + "]";
  std::string sums;
  for (int i = 0; i < up::kFileCount; ++i) {
    h.bodies[Url(up::kFiles[i])] = "abc";
    sums += std::string(kShaAbc) + "  " + up::kFiles[i] + "\n";
  }
  h.bodies[Url("SHA256SUMS.txt")] = sums;
}
static up::Env MakeEnv(FakeHttp& h, const fs::path& dir, const std::string& current) {
  up::Env e;
  e.get = [&h](const std::string& u, bool api, const up::Sink& s, std::string* err) { return h.Get(u, api, s, err); };
  e.dir = dir;
  e.repo = "o/r";
  e.channel = "netplay";
  e.current = current;
  return e;
}
static fs::path GameDir(const std::string& tag) {  // the three old files and a toml
  const fs::path d = NewDir(tag);
  for (int i = 0; i < 3; ++i) Write(d / up::kFiles[i], kOld[i]);
  Write(d / "dead_or_alive_4.toml", "t");
  return d;
}
static fs::path WithSuffix(const fs::path& d, int i, const char* suffix) { return d / (std::string(up::kFiles[i]) + suffix); }
// A check that must change nothing: the folder (no update_new left either) and the latch.
static void ExpectUnchanged(const char* what, const fs::path& d, const std::map<std::string, std::string>& d0) {
  const auto d1 = Snapshot(d);
  ++g_checks;
  if (d1 != d0 || up::RestartPending()) {
    std::printf("FAIL %s: changed (restart pending %d)\n  before: %s\n  after:  %s\n", what, int(up::RestartPending()),
                Describe(d0).c_str(), Describe(d1).c_str());
    ++g_failures;
  }
}
template <class F>
static bool WaitFor(F f, int ms) {
  const auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(ms)) {
    if (f()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return f();
}

// ---- 1-3: the values ----
static void TestValues() {
#if defined(_WIN32)
  CHECK(up::kIsWindows == true);
#else
  CHECK(up::kIsWindows == false);
#endif
  CHECK(up::rules::MayInstall(false) == false);
  CHECK(up::rules::MayInstall(true) == true);
  CHECK_EQ(up::kNoInstallText, "This build cannot update itself. Build the new version from source.");
  CHECK(up::Env{}.may_install == up::kIsWindows);
  CHECK(up::GameEnv().may_install == up::kIsWindows);  // GameEnv only fills a struct: nothing is asked
}

// ---- 4: a release build that may not install asks nothing and changes nothing ----
static void TestRefuseRelease() {
  up::ResetForTest();
  FakeHttp h;
  Serve(h);
  const fs::path d = GameDir("refuse_release");
  const auto d0 = Snapshot(d);
  up::Env env = MakeEnv(h, d, "netplay-v1.0.5");
  env.may_install = false;
  const up::Result r = up::CheckAndUpdate(up::Request{}, env);
  CHECK(!r.ok);
  CHECK_EQ(r.message, up::kNoInstallText);
  CHECK(r.tag.empty());
  CHECK_FOUND(r, up::Found::kFailed);
  CHECK(h.asked.empty());  // not even the release list
  ExpectUnchanged("refused release build", d, d0);
  CHECK(!up::OfferPending());
}

// ---- 5: a self-built game: neither the offer nor the confirming press ----
static void TestRefuseDev() {
  up::ResetForTest();
  FakeHttp h;
  Serve(h);
  const fs::path d = GameDir("refuse_dev");
  const auto d0 = Snapshot(d);
  up::Env env = MakeEnv(h, d, "dev");
  env.may_install = false;
  const up::Result r1 = up::CheckAndUpdate(up::Request{}, env);
  CHECK_EQ(r1.message, up::kNoInstallText);  // not OfferText
  CHECK(h.asked.empty());
  CHECK(!up::OfferPending());
  up::Request c;
  c.confirm = true;
  const up::Result r2 = up::CheckAndUpdate(c, env);
  CHECK_EQ(r2.message, up::kNoInstallText);
  CHECK(h.asked.empty());
  CHECK(!up::OfferPending());
  ExpectUnchanged("refused self-built game", d, d0);
  for (int i = 0; i < 3; ++i) CHECK(!Exists(WithSuffix(d, i, ".dev")));
  CHECK(!up::RestartPending());
}

// ---- 6: through Start, as the Online tab and the lobby seam reach it ----
static void TestRefuseThroughStart() {
  up::ResetForTest();
  // static: a check thread that outlived a timed-out wait must not reach a dead stack frame
  static FakeHttp h;
  static std::mutex mu;
  static std::vector<std::string> lines;
  static up::Result done_r;
  static std::atomic<bool> done_called{false};
  Serve(h);
  const fs::path d = GameDir("refuse_start");
  const auto d0 = Snapshot(d);
  up::Env env = MakeEnv(h, d, "netplay-v1.0.5");
  env.may_install = false;
  up::Request q;
  q.log = [](const std::string& l) {
    std::lock_guard<std::mutex> g(mu);
    lines.push_back(l);
  };
  q.done = [](const up::Result& r) {
    std::lock_guard<std::mutex> g(mu);
    done_r = r;
    done_called = true;
  };
  CHECK(up::Start(q, [env](const up::Request& r) { return up::CheckAndUpdate(r, env); }));
  CHECK(WaitFor([] { return !up::Busy(); }, 10000));
  CHECK_EQ(up::Message(), up::kNoInstallText);
  std::lock_guard<std::mutex> g(mu);
  const std::string want = "[doa4][UPDATE] " + std::string(up::kNoInstallText);
  CHECK(std::count(lines.begin(), lines.end(), want) == 1);
  CHECK(done_called.load());
  CHECK_FOUND(done_r, up::Found::kFailed);
  CHECK(!done_r.ok);
  CHECK(h.asked.empty());
  ExpectUnchanged("refused through Start", d, d0);
}

// ---- 7 (the control) and 10 (the refusal comes before the latch) ----
static void TestControlThenLatch() {
  up::ResetForTest();
  {  // 7: may_install = true installs the same release through the same kind of fake and folder
    FakeHttp h;
    Serve(h);
    const fs::path d = GameDir("control");
    up::Env env = MakeEnv(h, d, "netplay-v1.0.5");
    env.may_install = true;
    const up::Result r = up::CheckAndUpdate(up::Request{}, env);
    CHECK(r.ok);
    CHECK_EQ(r.message, up::UpdatedText(kTag, false));
    CHECK(h.asked.size() == 5);  // the list, the three files, SHA256SUMS.txt
    for (int i = 0; i < 3; ++i) CHECK(Holds(d / up::kFiles[i], "abc"));
    for (int i = 0; i < 3; ++i) CHECK(Holds(WithSuffix(d, i, ".old"), kOld[i]));
    CHECK(!Exists(d / up::kStagingName));
    CHECK(up::RestartPending());
  }
  {  // 10: no ResetForTest, so the "already installed" latch is set; the refusal still answers first
    CHECK(up::RestartPending());
    FakeHttp h;
    Serve(h);
    const fs::path d = GameDir("latch");
    up::Env env = MakeEnv(h, d, "netplay-v1.0.5");
    env.may_install = false;
    const up::Result r = up::CheckAndUpdate(up::Request{}, env);
    CHECK_EQ(r.message, up::kNoInstallText);  // not UpdatedText
    CHECK(r.tag.empty());
    CHECK(h.asked.empty());
  }
  up::ResetForTest();
}

// ---- 8: CheckOnly still says that a newer release exists ----
static void TestCheckOnlyAllowed() {
  up::ResetForTest();
  FakeHttp h;
  Serve(h);
  const fs::path d = GameDir("check_only");
  const auto d0 = Snapshot(d);
  up::Env env = MakeEnv(h, d, "netplay-v1.0.5");
  env.may_install = false;
  const up::Result r = up::CheckOnly(env);
  CHECK_FOUND(r, up::Found::kNewer);
  CHECK_EQ(r.tag, kTag);
  CHECK_EQ(r.message, "Release netplay-v1.0.6 is available");
  CHECK(h.asked.size() == 1 && h.asked[0] == kApi);
  ExpectUnchanged("check only", d, d0);
}

// ---- 9: CleanUpAtStart ----
static fs::path Planted(const std::string& tag) {  // three files, three .old files, update_new holding a file
  const fs::path d = NewDir(tag);
  for (int i = 0; i < 3; ++i) {
    Write(d / up::kFiles[i], kOld[i]);
    Write(WithSuffix(d, i, ".old"), "old");
  }
  std::error_code ec;
  fs::create_directories(d / up::kStagingName, ec);
  Write(d / up::kStagingName / "dead_or_alive_4.exe", "half a download");
  return d;
}
static void TestCleanUp() {
  const fs::path d = Planted("cleanup");
  up::CleanUpAtStart(d, false);  // may not install: nothing to clean up, nothing touched
  for (int i = 0; i < 3; ++i) {
    CHECK(Exists(d / up::kFiles[i]));
    CHECK(Exists(WithSuffix(d, i, ".old")));
  }
  CHECK(Exists(d / up::kStagingName / "dead_or_alive_4.exe"));
  up::CleanUpAtStart(d, true);  // the control: the same folder is cleaned
  for (int i = 0; i < 3; ++i) {
    CHECK(!Exists(WithSuffix(d, i, ".old")));
    CHECK(Exists(d / up::kFiles[i]));
  }
  CHECK(!Exists(d / up::kStagingName));
  const fs::path d2 = Planted("cleanup_default");
  up::CleanUpAtStart(d2);  // as the game calls it: the build's own rule
  for (int i = 0; i < 3; ++i) {
    CHECK(Exists(WithSuffix(d2, i, ".old")) == !up::kIsWindows);
    CHECK(Exists(d2 / up::kFiles[i]));
  }
  CHECK(Exists(d2 / up::kStagingName) == !up::kIsWindows);
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::error_code ec;
  const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
  g_root = fs::temp_directory_path(ec) / ("doa4_updater_refusal_" + std::to_string(stamp));
  fs::create_directories(g_root, ec);
  CHECK(!ec && Exists(g_root));
  TestValues();
  TestRefuseRelease();
  TestRefuseDev();
  TestRefuseThroughStart();
  TestControlThenLatch();
  TestCheckOnlyAllowed();
  TestCleanUp();
  fs::remove_all(g_root, ec);
  CHECK(!ec && !Exists(g_root));
  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
