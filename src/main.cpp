/* main.cpp - thin CLI over the same core the plugin uses. */
#include "devinfo.h"
#include "devinfo_core.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#define ISATTY(f) _isatty(_fileno(f))
#else
#include <unistd.h>
#define ISATTY(f) isatty(fileno(f))
#endif

namespace {

void usage() {
  std::fputs(
      "devinfo " DEVINFO_VERSION_STRING " - hardware & accelerator census\n"
      "\n"
      "usage: devinfo [options]\n"
      "  --json        machine-readable output (default when not a tty)\n"
      "  --table       human-readable output (default on a tty)\n"
      "  --bench       add a memory bandwidth micro-benchmark\n"
      "  -v, --version print version\n"
      "  -h, --help    this text\n"
      "\n"
      "Started by double-click, it also writes devinfo-report.txt and\n"
      "devinfo-report.json next to the executable and waits for Enter.\n",
      stdout);
}

bool write_file(const std::string& path, const std::string& data) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  size_t n = std::fwrite(data.data(), 1, data.size(), f);
  std::fclose(f);
  return n == data.size();
}

#if defined(_WIN32)
/* A console program started from Explorer gets a console of its own with
 * exactly one process attached to it. Anything started from cmd or PowerShell
 * shares that console with at least the shell, so the count is >= 2. This is
 * how we tell "user double-clicked" from "user is scripting us", without
 * changing behaviour for scripts. */
bool launched_by_double_click() {
  DWORD pids[4] = {0};
  return GetConsoleProcessList(pids, 4) <= 1;
}

std::string exe_directory() {
  char buf[MAX_PATH] = {0};
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return std::string();
  char* slash = std::strrchr(buf, '\\');
  if (!slash) return std::string();
  *slash = '\0';
  return std::string(buf);
}

std::string temp_directory() {
  char buf[MAX_PATH] = {0};
  DWORD n = GetTempPathA(MAX_PATH, buf);
  if (n == 0 || n > MAX_PATH) return std::string();
  std::string s(buf);
  while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
  return s;
}
#endif

}  /* namespace */

int main(int argc, char** argv) {
  uint32_t flags = DEVINFO_FAST;
  int mode = 0;  /* 0 auto, 1 json, 2 table */

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (!std::strcmp(a, "--json"))         mode = 1;
    else if (!std::strcmp(a, "--table"))   mode = 2;
    else if (!std::strcmp(a, "--bench"))   flags |= DEVINFO_BENCH;
    else if (!std::strcmp(a, "-v") || !std::strcmp(a, "--version")) {
      std::printf("devinfo %s (abi %u)\n", DEVINFO_VERSION_STRING, DEVINFO_ABI_VERSION);
      return 0;
    } else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "devinfo: unknown option '%s'\n", a);
      usage();
      return 2;
    }
  }

  if (mode == 0) mode = ISATTY(stdout) ? 2 : 1;

  devinfo::Report r = devinfo::probe(flags);

#if defined(_WIN32)
  /* The console we get from Explorer is GBK on a Chinese install; the report is
   * UTF-8, so any non-ASCII name would come out as mojibake. */
  bool interactive = launched_by_double_click();
  if (interactive) SetConsoleOutputCP(CP_UTF8);
#endif

  std::string table = devinfo::to_table(r);
  std::string json = (mode == 1) ? table : devinfo::to_json(r);
  const std::string& shown = (mode == 1) ? json : table;
  std::fwrite(shown.data(), 1, shown.size(), stdout);

#if defined(_WIN32)
  if (interactive) {
    /* Nothing to keep the window open otherwise: the report would be gone
     * before anyone could read it, which is exactly what happens on a machine
     * where you cannot open a shell to run this by hand. */
    std::string dir = exe_directory();
    std::string txt = dir + "\\devinfo-report.txt";
    std::string jsn = dir + "\\devinfo-report.json";
    bool ok_txt = !dir.empty() && write_file(txt, table);
    bool ok_jsn = !dir.empty() && write_file(jsn, devinfo::to_json(r));
    if (!ok_txt && !ok_jsn) {  /* exe dir may be read-only */
      dir = temp_directory();
      txt = dir + "\\devinfo-report.txt";
      jsn = dir + "\\devinfo-report.json";
      ok_txt = !dir.empty() && write_file(txt, table);
      ok_jsn = !dir.empty() && write_file(jsn, devinfo::to_json(r));
    }

    std::fputs("\n", stdout);
    if (ok_txt) std::printf("report written: %s\n", txt.c_str());
    if (ok_jsn) std::printf("report written: %s\n", jsn.c_str());
    if (!ok_txt && !ok_jsn)
      std::fputs("could not write a report file next to the executable\n", stdout);
    std::fputs("\npress Enter to close ...", stdout);
    std::fflush(stdout);
    std::getchar();
    return 0;
  }
#endif
  return 0;
}
