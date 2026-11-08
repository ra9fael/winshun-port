// TestTree.exe — a three-level process tree for the manual E2E of killTree.
//
// Run without arguments to start the root: the root spawns one child, the
// child spawns one grandchild. Every level prints its level and PID, then
// sleeps forever, so killing the root alone leaves the descendants running —
// exactly the situation `Ctrl+Enter` (kill tree) exists for.
//
// Expected console output, one line per level:
//   [TestTree] level 0 pid 1234
//   [TestTree] level 1 pid 2345
//   [TestTree] level 2 pid 3456

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

static void spawnNextLevel(int level)
{
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0)
        return;
    std::wstring cmd = L"\"" + std::wstring(self) + L"\" " + std::to_wstring(level + 1);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    // A new console per level keeps the tree visible: all three windows
    // disappearing at once is the manual check for a successful tree kill.
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr,
                        nullptr, &si, &pi)) {
        wprintf(L"[TestTree] level %d pid %lu: spawn of level %d failed (%lu)\n", level,
                GetCurrentProcessId(), level + 1, GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

int wmain(int argc, wchar_t** argv)
{
    int level = 0;
    if (argc > 1)
        level = _wtoi(argv[1]);
    if (level < 0 || level > 2)
        level = 0;
    if (level < 2)
        spawnNextLevel(level);
    wprintf(L"[TestTree] level %d pid %lu\n", level, GetCurrentProcessId());
    fflush(stdout);
    for (;;)
        Sleep(60000);
}
