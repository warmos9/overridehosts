// overridehosts.cpp
//
// Per-process hosts override wrapper using LD_PRELOAD.
// No root required.
//
// Mapping format everywhere:
//   host:ip
//   IPv6 recommended as host:[2001:db8::1]
//
// Mapping sources (merged in order):
//   1) OVERRIDEHOSTS environment variable (comma / whitespace separated)
//   2) CLI args before "--"
// CLI mappings come last and therefore win.
//
// Preload library selection:
//   - liboverridehosts-musl.so if musl loader is present
//   - liboverridehosts-glibc.so otherwise
//   - OVERRIDEHOSTS_SO overrides everything
//
// Usage:
//   ./overridehosts "example:192.168.0.1" -- ping example
//   OVERRIDEHOSTS="db:10.0.0.10,redis:10.0.0.11" ./overridehosts -- wget http://db/

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>
#include <limits.h>

static void die(const std::string& msg) {
  std::cerr << "overridehosts: " << msg << "\n";
  std::exit(1);
}

static bool exists(const std::string& path) {
  return ::access(path.c_str(), R_OK) == 0;
}

static std::string get_exe_dir() {
  char buf[PATH_MAX] = {0};
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) die("cannot read /proc/self/exe");
  buf[n] = 0;
  std::string full(buf);
  auto pos = full.find_last_of('/');
  return (pos == std::string::npos) ? "." : full.substr(0, pos);
}

static bool looks_like_mapping(const std::string& s) {
  return !s.empty() && s[0] != '-' && s.find(':') != std::string::npos;
}

static void parse_env_overridehosts(std::vector<std::string>& out) {
  const char* env = std::getenv("OVERRIDEHOSTS");
  if (!env || !*env) return;

  std::string s(env);
  std::string cur;

  auto flush = [&]() {
    if (!cur.empty()) {
      if (looks_like_mapping(cur)) out.push_back(cur);
      cur.clear();
    }
  };

  for (char c : s) {
    if (c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
      flush();
    else
      cur.push_back(c);
  }
  flush();
}

static std::string join_csv(const std::vector<std::string>& v) {
  std::ostringstream oss;
  for (size_t i = 0; i < v.size(); i++) {
    if (i) oss << ",";
    oss << v[i];
  }
  return oss.str();
}

static bool is_musl_runtime() {
  return
    ::access("/lib/ld-musl-x86_64.so.1", R_OK) == 0 ||
    ::access("/lib/ld-musl-aarch64.so.1", R_OK) == 0 ||
    ::access("/lib/ld-musl-armhf.so.1", R_OK) == 0 ||
    ::access("/lib/ld-musl-i386.so.1", R_OK) == 0 ||
    ::access("/lib/ld-musl-riscv64.so.1", R_OK) == 0;
}

static std::string select_preload_so(const std::string& exe_dir) {
  if (const char* p = std::getenv("OVERRIDEHOSTS_SO"); p && *p)
    return std::string(p);

  return exe_dir + (is_musl_runtime()
    ? "/liboverridehosts-musl.so"
    : "/liboverridehosts-glibc.so");
}

static void setenv_or_die(const char* k, const std::string& v) {
  if (::setenv(k, v.c_str(), 1) != 0)
    die(std::string("setenv(") + k + ") failed: " + std::strerror(errno));
}

int main(int argc, char** argv) {
  std::vector<std::string> mappings;
  parse_env_overridehosts(mappings);

  int sep = -1;
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--") { sep = i; break; }
    if (!looks_like_mapping(argv[i]))
      die(std::string("unexpected argument before '--': ") + argv[i]);
    mappings.push_back(argv[i]);
  }

  if (sep == -1 || sep + 1 >= argc) {
    std::cerr
      << "Usage:\n"
      << "  " << argv[0] << " \"host:ip\" [\"host2:ip2\" ...] -- <command> [args...]\n"
      << "  OVERRIDEHOSTS=\"host:ip,host2:ip2\" " << argv[0] << " -- <command>\n";
    return 2;
  }

  if (mappings.empty())
    die("no mappings provided (use args and/or OVERRIDEHOSTS)");

  std::string exe_dir = get_exe_dir();
  std::string so_path = select_preload_so(exe_dir);

  if (!exists(so_path)) {
    die(
      "preload library not found:\n  " + so_path +
      "\nExpected:\n  " + exe_dir + "/liboverridehosts-glibc.so"
      "\n  " + exe_dir + "/liboverridehosts-musl.so"
      "\nOr set OVERRIDEHOSTS_SO"
    );
  }

  setenv_or_die("OVERRIDEHOSTS", join_csv(mappings));

  {
    const char* old = std::getenv("LD_PRELOAD");
    std::string preload = so_path;
    if (old && *old) preload += std::string(" ") + old;
    setenv_or_die("LD_PRELOAD", preload);
  }

  execvp(argv[sep + 1], &argv[sep + 1]);
  die(std::string("execvp failed: ") + std::strerror(errno));
}
