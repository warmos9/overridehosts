#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <netdb.h>
#include <sys/socket.h>

#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

static std::once_flag g_init_once;
static std::unordered_map<std::string, std::string> g_map; // hostname(lower)->ip string
static std::atomic<bool> g_inited{false};

static std::string to_lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

static std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) a++;
  while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

static void parse_map_env() {
  const char* env = std::getenv("OVERRIDEHOSTS");
  if (!env || !*env) {
    g_inited.store(true);
    return;
  }

  std::string all(env);
  size_t i = 0;
  while (i < all.size()) {
    size_t j = all.find(',', i);
    if (j == std::string::npos) j = all.size();
    std::string item = trim(all.substr(i, j - i));
    i = j + 1;
    if (item.empty()) continue;

    // Split on first ':'
    size_t c = item.find(':');
    if (c == std::string::npos || c == 0 || c + 1 >= item.size()) continue;

    std::string host = to_lower(trim(item.substr(0, c)));
    std::string ip   = trim(item.substr(c + 1));

    // If IPv6 is in [..], strip brackets.
    if (!ip.empty() && ip.front() == '[' && ip.back() == ']' && ip.size() >= 3) {
      ip = ip.substr(1, ip.size() - 2);
    }

    if (!host.empty() && !ip.empty()) g_map[host] = ip;
  }

  g_inited.store(true);
}

static void ensure_inited() {
  std::call_once(g_init_once, []() { parse_map_env(); });
}

static bool lookup_ip_for(const char* node, std::string& out_ip) {
  if (!node || !*node) return false;
  ensure_inited();
  auto it = g_map.find(to_lower(std::string(node)));
  if (it == g_map.end()) return false;
  out_ip = it->second;
  return true;
}

static int make_addrinfo_list(const std::string& ip, const struct addrinfo* hints, struct addrinfo** res) {
  if (!res) return EAI_FAIL;
  *res = nullptr;

  int family = hints ? hints->ai_family : AF_UNSPEC;
  int socktype = hints ? hints->ai_socktype : 0;
  int protocol = hints ? hints->ai_protocol : 0;

  // Determine if ip is v4 or v6
  in_addr  a4{};
  in6_addr a6{};
  bool is4 = inet_pton(AF_INET, ip.c_str(), &a4) == 1;
  bool is6 = inet_pton(AF_INET6, ip.c_str(), &a6) == 1;
  if (!is4 && !is6) return EAI_NONAME;

  // Respect family hint if set
  if (family == AF_INET && !is4) return EAI_NONAME;
  if (family == AF_INET6 && !is6) return EAI_NONAME;

  // If AF_UNSPEC and ip parses as v4, return v4; if v6, return v6.
  int out_family = is4 ? AF_INET : AF_INET6;

  // Create one addrinfo node
  addrinfo* ai = (addrinfo*)calloc(1, sizeof(addrinfo));
  if (!ai) return EAI_MEMORY;

  ai->ai_family = out_family;
  ai->ai_socktype = socktype;
  ai->ai_protocol = protocol;

  if (out_family == AF_INET) {
    sockaddr_in* sa = (sockaddr_in*)calloc(1, sizeof(sockaddr_in));
    if (!sa) { free(ai); return EAI_MEMORY; }
    sa->sin_family = AF_INET;
    sa->sin_addr = a4;
    ai->ai_addr = (sockaddr*)sa;
    ai->ai_addrlen = sizeof(sockaddr_in);
  } else {
    sockaddr_in6* sa = (sockaddr_in6*)calloc(1, sizeof(sockaddr_in6));
    if (!sa) { free(ai); return EAI_MEMORY; }
    sa->sin6_family = AF_INET6;
    sa->sin6_addr = a6;
    ai->ai_addr = (sockaddr*)sa;
    ai->ai_addrlen = sizeof(sockaddr_in6);
  }

  ai->ai_next = nullptr;
  *res = ai;
  return 0;
}

// --- getaddrinfo override ---
using real_getaddrinfo_t = int(*)(const char*, const char*, const struct addrinfo*, struct addrinfo**);

extern "C" int getaddrinfo(const char* node, const char* service,
                           const struct addrinfo* hints, struct addrinfo** res) {
  static real_getaddrinfo_t real_getaddrinfo =
      (real_getaddrinfo_t)dlsym(RTLD_NEXT, "getaddrinfo");

  std::string ip;
  if (lookup_ip_for(node, ip)) {
    // We ignore "service" here; callers typically call getaddrinfo with service and then set port later.
    // If needed, you can parse service to port and set it in sockaddr.
    return make_addrinfo_list(ip, hints, res);
  }

  return real_getaddrinfo ? real_getaddrinfo(node, service, hints, res) : EAI_FAIL;
}

// --- gethostbyname override (legacy) ---
using real_gethostbyname_t = struct hostent*(*)(const char*);
using real_gethostbyname2_t = struct hostent*(*)(const char*, int);

static thread_local hostent g_he{};
static thread_local std::vector<unsigned char> g_addr_storage;
static thread_local std::vector<char*> g_addr_list;
static thread_local std::string g_name;
static thread_local std::string g_ip_str;

static hostent* make_hostent_v4(const std::string& name, const std::string& ip) {
  in_addr a4{};
  if (inet_pton(AF_INET, ip.c_str(), &a4) != 1) return nullptr;

  g_name = name;
  g_ip_str = ip;

  g_addr_storage.resize(sizeof(in_addr));
  std::memcpy(g_addr_storage.data(), &a4, sizeof(in_addr));

  g_addr_list.clear();
  g_addr_list.push_back((char*)g_addr_storage.data());
  g_addr_list.push_back(nullptr);

  std::memset(&g_he, 0, sizeof(g_he));
  g_he.h_name = (char*)g_name.c_str();
  g_he.h_aliases = nullptr;
  g_he.h_addrtype = AF_INET;
  g_he.h_length = sizeof(in_addr);
  g_he.h_addr_list = g_addr_list.data();
  return &g_he;
}

extern "C" struct hostent* gethostbyname(const char* name) {
  static real_gethostbyname_t real_gethostbyname =
      (real_gethostbyname_t)dlsym(RTLD_NEXT, "gethostbyname");

  std::string ip;
  if (lookup_ip_for(name, ip)) {
    // This legacy API only supports v4 cleanly. If you need v6, use getaddrinfo in your program.
    return make_hostent_v4(name ? name : "", ip);
  }

  return real_gethostbyname ? real_gethostbyname(name) : nullptr;
}

extern "C" struct hostent* gethostbyname2(const char* name, int af) {
  static real_gethostbyname2_t real_gethostbyname2 =
      (real_gethostbyname2_t)dlsym(RTLD_NEXT, "gethostbyname2");

  std::string ip;
  if (lookup_ip_for(name, ip)) {
    if (af == AF_INET) return make_hostent_v4(name ? name : "", ip);
    // For AF_INET6 callers, rely on getaddrinfo path; return nullptr here.
    return nullptr;
  }

  return real_gethostbyname2 ? real_gethostbyname2(name, af) : nullptr;
}
