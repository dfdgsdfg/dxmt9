// tool-winctl: Win32-side GUI automation helper for Wine harness runs.
//
// The Wine window is a Windows window first: automating it through Win32
// messages inside the prefix is deterministic, needs no macOS Accessibility
// permission, no screen coordinates, and works while the window is hidden
// behind other macOS windows. This tool is run as
//   wine tool-winctl-x64.exe <subcommand> ...
// in the same prefix as the target app; wineserver routes the messages.
//
// Subcommands:
//   dump   [--window SUBSTR]                 list top-level windows; for each
//                                            match (or all when omitted) dump
//                                            the child control tree
//   wait   --window SUBSTR [--timeout-ms N]  poll until a matching windowexists
//   click  --window SUBSTR (--button TEXT | --control-id N)
//                                            BM_CLICK on a standard control
//   clickxy --window SUBSTR --x N --y N      WM_LBUTTONDOWN/UP at client
//                                            coords (custom-drawn UIs)
//   click-controlxy --window SUBSTR --control-id N --x N --y N
//                                            WM_LBUTTONDOWN/UP at child-client
//                                            coords (tree/list controls)
//   settext --window SUBSTR --control-id N --text S
//   command --window SUBSTR --id N           WM_COMMAND (menu/accelerator id)
//   close  --window SUBSTR                   WM_CLOSE
//
// Window matching is a case-insensitive substring over the top-level window
// title; button-text matching additionally strips '&' mnemonics. Exit codes:
// 0 success, 1 target not found, 2 usage error.

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

std::string narrow(const std::wstring &w) {
  if (w.empty()) {
    return {};
  }
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string out(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) {
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
  }
  return out;
}

std::wstring widen(const char *s) {
  if (!s || !*s) {
    return {};
  }
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  std::wstring out(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) {
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
  }
  return out;
}

std::wstring lowered(std::wstring value) {
  for (auto &ch : value) {
    ch = static_cast<wchar_t>(towlower(ch));
  }
  return value;
}

std::wstring strippedMnemonics(const std::wstring &value) {
  std::wstring out;
  out.reserve(value.size());
  for (wchar_t ch : value) {
    if (ch != L'&') {
      out.push_back(ch);
    }
  }
  return out;
}

bool containsInsensitive(const std::wstring &haystack, const std::wstring &needle) {
  if (needle.empty()) {
    return true;
  }
  return lowered(haystack).find(lowered(needle)) != std::wstring::npos;
}

std::wstring windowText(HWND hwnd) {
  // GetWindowTextW reads the caption for same-process windows only; for
  // cross-process windows it still works for top-level captions, but child
  // control text needs WM_GETTEXT. Use WM_GETTEXT with a timeout everywhere
  // so a hung target cannot wedge the tool.
  wchar_t buffer[1024] = {0};
  DWORD_PTR length = 0;
  if (!SendMessageTimeoutW(hwnd, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG, 1000, &length)) {
    return {};
  }
  if (length == 0) {
    return {};
  }
  DWORD_PTR copied = 0;
  if (!SendMessageTimeoutW(hwnd, WM_GETTEXT, 1023, reinterpret_cast<LPARAM>(buffer), SMTO_ABORTIFHUNG,
                           1000, &copied)) {
    return {};
  }
  return std::wstring(buffer);
}

std::wstring windowClass(HWND hwnd) {
  wchar_t buffer[256] = {0};
  GetClassNameW(hwnd, buffer, 255);
  return std::wstring(buffer);
}

struct TopWindowQuery {
  std::wstring needle;
  std::vector<HWND> matches;
};

BOOL CALLBACK collectTopWindows(HWND hwnd, LPARAM lparam) {
  auto *query = reinterpret_cast<TopWindowQuery *>(lparam);
  std::wstring title = windowText(hwnd);
  if (title.empty() && !query->needle.empty()) {
    return TRUE;
  }
  if (containsInsensitive(title, query->needle)) {
    query->matches.push_back(hwnd);
  }
  return TRUE;
}

std::vector<HWND> findTopWindows(const std::wstring &needle) {
  TopWindowQuery query{needle, {}};
  EnumWindows(collectTopWindows, reinterpret_cast<LPARAM>(&query));
  return query.matches;
}

void printWindowLine(HWND hwnd, int depth) {
  RECT rect{};
  GetWindowRect(hwnd, &rect);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  int controlId = GetDlgCtrlID(hwnd);
  std::string indent(static_cast<size_t>(depth) * 2, ' ');
  std::printf("%shwnd=0x%08llx class=\"%s\" id=%d pid=%lu visible=%d enabled=%d rect=%ld,%ld,%ld,%ld text=\"%s\"\n",
              indent.c_str(), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)),
              narrow(windowClass(hwnd)).c_str(), controlId, static_cast<unsigned long>(pid),
              IsWindowVisible(hwnd) ? 1 : 0, IsWindowEnabled(hwnd) ? 1 : 0, rect.left, rect.top,
              rect.right, rect.bottom, narrow(windowText(hwnd)).c_str());
}

struct ChildDumpState {
  int depth;
};

BOOL CALLBACK dumpChild(HWND hwnd, LPARAM lparam);

void dumpChildrenRecursive(HWND parent, int depth) {
  ChildDumpState state{depth};
  EnumChildWindows(parent, dumpChild, reinterpret_cast<LPARAM>(&state));
}

BOOL CALLBACK dumpChild(HWND hwnd, LPARAM lparam) {
  auto *state = reinterpret_cast<ChildDumpState *>(lparam);
  // EnumChildWindows already recurses into grandchildren; print flat with the
  // caller's depth so the listing stays complete without double recursion.
  printWindowLine(hwnd, state->depth);
  return TRUE;
}

struct ChildSearch {
  std::wstring buttonText;
  int controlId = 0;
  bool byId = false;
  HWND found = nullptr;
};

BOOL CALLBACK matchChild(HWND hwnd, LPARAM lparam) {
  auto *search = reinterpret_cast<ChildSearch *>(lparam);
  if (search->byId) {
    if (GetDlgCtrlID(hwnd) == search->controlId) {
      search->found = hwnd;
      return FALSE;
    }
    return TRUE;
  }
  std::wstring text = strippedMnemonics(windowText(hwnd));
  if (!text.empty() && containsInsensitive(text, strippedMnemonics(search->buttonText))) {
    search->found = hwnd;
    return FALSE;
  }
  return TRUE;
}

const char *argValue(int argc, char **argv, const char *name) {
  for (int i = 2; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], name) == 0) {
      return argv[i + 1];
    }
  }
  return nullptr;
}

bool hasArg(int argc, char **argv, const char *name) {
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], name) == 0) {
      return true;
    }
  }
  return false;
}

int usage() {
  std::fprintf(stderr,
               "usage: tool-winctl <dump|wait|click|clickxy|click-controlxy|settext|command|close> [options]\n"
               "  dump    [--window SUBSTR]\n"
               "  wait    --window SUBSTR [--timeout-ms N]\n"
               "  click   --window SUBSTR (--button TEXT | --control-id N)\n"
               "  clickxy --window SUBSTR --x N --y N\n"
               "  click-controlxy --window SUBSTR --control-id N --x N --y N\n"
               "  settext --window SUBSTR --control-id N --text S\n"
               "  command --window SUBSTR --id N\n"
               "  close   --window SUBSTR\n");
  return 2;
}

HWND requireWindow(const std::wstring &needle) {
  std::vector<HWND> matches = findTopWindows(needle);
  if (matches.empty()) {
    std::fprintf(stderr, "error: no top-level window matching \"%s\"\n", narrow(needle).c_str());
    return nullptr;
  }
  if (matches.size() > 1) {
    std::fprintf(stderr, "warn: %zu windows match \"%s\"; using the first\n", matches.size(),
                 narrow(needle).c_str());
  }
  return matches.front();
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    return usage();
  }
  const std::string cmd = argv[1];
  const char *windowArg = argValue(argc, argv, "--window");
  const std::wstring needle = widen(windowArg);

  if (cmd == "dump") {
    std::vector<HWND> matches = findTopWindows(needle);
    if (windowArg && matches.empty()) {
      std::fprintf(stderr, "error: no top-level window matching \"%s\"\n", windowArg);
      return 1;
    }
    for (HWND hwnd : matches) {
      if (!windowArg && !IsWindowVisible(hwnd)) {
        continue;
      }
      printWindowLine(hwnd, 0);
      dumpChildrenRecursive(hwnd, 1);
    }
    return 0;
  }

  if (cmd == "wait") {
    if (!windowArg) {
      return usage();
    }
    const char *timeoutArg = argValue(argc, argv, "--timeout-ms");
    DWORD timeoutMs = timeoutArg ? static_cast<DWORD>(std::strtoul(timeoutArg, nullptr, 10)) : 30000;
    DWORD waited = 0;
    while (waited <= timeoutMs) {
      std::vector<HWND> matches = findTopWindows(needle);
      if (!matches.empty()) {
        printWindowLine(matches.front(), 0);
        return 0;
      }
      Sleep(250);
      waited += 250;
    }
    std::fprintf(stderr, "error: timed out waiting for window \"%s\"\n", windowArg);
    return 1;
  }

  if (!windowArg) {
    return usage();
  }
  HWND top = requireWindow(needle);
  if (!top) {
    return 1;
  }

  if (cmd == "click") {
    ChildSearch search;
    if (const char *idArg = argValue(argc, argv, "--control-id")) {
      search.byId = true;
      search.controlId = std::atoi(idArg);
    } else if (const char *buttonArg = argValue(argc, argv, "--button")) {
      search.buttonText = widen(buttonArg);
    } else {
      return usage();
    }
    EnumChildWindows(top, matchChild, reinterpret_cast<LPARAM>(&search));
    if (!search.found) {
      std::fprintf(stderr, "error: no matching child control\n");
      return 1;
    }
    printWindowLine(search.found, 0);
    // Post instead of send: a click that opens a modal flow (e.g. 3DMark06's
    // Run button) would block a synchronous send until the modal finishes,
    // making success indistinguishable from a hang.
    if (!PostMessageW(search.found, BM_CLICK, 0, 0)) {
      std::fprintf(stderr, "error: BM_CLICK post failed\n");
      return 1;
    }
    return 0;
  }

  if (cmd == "clickxy") {
    const char *xArg = argValue(argc, argv, "--x");
    const char *yArg = argValue(argc, argv, "--y");
    if (!xArg || !yArg) {
      return usage();
    }
    LPARAM point = MAKELPARAM(std::atoi(xArg), std::atoi(yArg));
    // Post the pair instead of sending: custom-drawn UIs often run modal work
    // straight from the button-up handler, and a synchronous send would block
    // this tool for the whole benchmark run.
    PostMessageW(top, WM_LBUTTONDOWN, MK_LBUTTON, point);
    PostMessageW(top, WM_LBUTTONUP, 0, point);
    return 0;
  }

  if (cmd == "click-controlxy") {
    const char *idArg = argValue(argc, argv, "--control-id");
    const char *xArg = argValue(argc, argv, "--x");
    const char *yArg = argValue(argc, argv, "--y");
    if (!idArg || !xArg || !yArg) {
      return usage();
    }
    ChildSearch search;
    search.byId = true;
    search.controlId = std::atoi(idArg);
    EnumChildWindows(top, matchChild, reinterpret_cast<LPARAM>(&search));
    if (!search.found) {
      std::fprintf(stderr, "error: no child control with id %s\n", idArg);
      return 1;
    }
    LPARAM point = MAKELPARAM(std::atoi(xArg), std::atoi(yArg));
    PostMessageW(search.found, WM_LBUTTONDOWN, MK_LBUTTON, point);
    PostMessageW(search.found, WM_LBUTTONUP, 0, point);
    return 0;
  }

  if (cmd == "settext") {
    const char *idArg = argValue(argc, argv, "--control-id");
    const char *textArg = argValue(argc, argv, "--text");
    if (!idArg || !textArg) {
      return usage();
    }
    ChildSearch search;
    search.byId = true;
    search.controlId = std::atoi(idArg);
    EnumChildWindows(top, matchChild, reinterpret_cast<LPARAM>(&search));
    if (!search.found) {
      std::fprintf(stderr, "error: no child control with id %s\n", idArg);
      return 1;
    }
    std::wstring text = widen(textArg);
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(search.found, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()),
                             SMTO_ABORTIFHUNG, 5000, &result)) {
      std::fprintf(stderr, "error: WM_SETTEXT timed out\n");
      return 1;
    }
    return 0;
  }

  if (cmd == "command") {
    const char *idArg = argValue(argc, argv, "--id");
    if (!idArg) {
      return usage();
    }
    PostMessageW(top, WM_COMMAND, MAKEWPARAM(std::atoi(idArg), 0), 0);
    return 0;
  }

  if (cmd == "close") {
    PostMessageW(top, WM_CLOSE, 0, 0);
    return 0;
  }

  (void)hasArg;
  return usage();
}
