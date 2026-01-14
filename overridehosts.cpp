// overridehosts.cpp
// Usage:
//   OVERRIDEHOSTS="host1:1.2.3.4,host2:10.0.0.5" ./overridehosts -- ping host1
//   OVERRIDEHOSTS="host1:1.2.3.4 host2:10.0.0.5" ./overridehosts -- ping host2
//   ./overridehosts "host1:1.2.3.4" -- ping host1
//   OVERRIDEHOSTS="host1:1.2.3.4" ./overridehosts "host1:9.9.9.9" -- ping host1   # CLI wins

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

static bool is_file(const std::string& path) {
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

static std::string join_csv(const std::vector<std::string>& v) {
  std::ostringstream oss;
  for (size_t i = 0; i < v.size(); i++) {
    if (i) oss << ",";
    oss << v[i];
  }
  return oss.str();
}

static void parse_env_overridehosts(std::vector<std::string>& out) {
  const char* env = std::getenv("OVERRIDEHOSTS");
  if (!env || !*env) return;

  // Split by comma or whitespace
  std::string s(env);
  std::string cur;
  auto flush = [&]() {
    if (!cur.empty()) {
      if (looks_like_mapping(cur)) out.push_back(cur);
      cur.clear();
    }
  };

  for (char ch : s) {
    if (ch == ',' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
      flush();
    } else {
      cur.push_back(ch);
    }
  }
  flush();
}

int main(int argc, char** argv) {
  // Collect mappings from env first (so CLI can override by being appended later)
  std::vector<std::string> mappings;
  parse_env_overridehosts(mappings);

  // Parse argv for mappings until "--"
  int sep = -1;
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--") { sep = i; break; }
    if (!looks_like_mapping(argv[i]))
      die(std::string("unexpected argument before '--': ") + argv[i]);
    mappings.push_back(argv[i]);
  }

  if (sep == -1) {
    std::cerr
      << "Usage:\n"
      << "  " << argv[0] << " \"host:ip\" [\"host2:ip2\" ...] -- <command> [args...]\n"
      << "Or:\n"
      << "  OVERRIDEHOSTS=\"host:ip,host2:ip2\" " << argv[0] << " -- <command> [args...]\n";
    return 2;
  }
  if (sep + 1 >= argc) die("missing command after '--'");
  if (mappings.empty()) die("no mappings provided (use args and/or OVERRIDEHOSTS env)");

  // Locate preload library
  std::string exe_dir = get_exe_dir();
  std::string so_path = exe_dir + "/liboverridehosts.so";
  if (!is_file(so_path)) {
    const char* env_so = std::getenv("OVERRIDEHOSTS_SO");
    if (env_so && *env_so) so_path = env_so;
  }
  if (!is_file(so_path))
    die("cannot find liboverridehosts.so (or set OVERRIDEHOSTS_SO)");

  // Export map for the preload lib
  std::string map_csv = join_csv(mappings);
  if (setenv("OVERRIDEHOSTS", map_csv.c_str(), 1) != 0)
    die(std::string("setenv(OVERRIDEHOSTS) failed: ") + std::strerror(errno));

  // Setup LD_PRELOAD
  const char* old = std::getenv("LD_PRELOAD");
  std::string preload = so_path;
  if (old && *old) preload += std::string(" ") + old;

  if (setenv("LD_PRELOAD", preload.c_str(), 1) != 0)
    die(std::string("setenv(LD_PRELOAD) failed: ") + std::strerror(errno));

  // Exec
  execvp(argv[sep + 1], &argv[sep + 1]);
  die(std::string("execvp failed: ") + std::strerror(errno));
}
