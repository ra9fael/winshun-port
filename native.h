// native.h — ra9fael.port's Win32 side: the socket tables (iphlpapi), the
// process names and paths, the kills, the WMI command line, and the UAC
// taskkill. All the OS calls live here; the tests stay in logic.h.

#pragma once

// winsock2.h replaces the ancient winsock.h; define WIN32_LEAN_AND_MEAN (the
// CMake build sets it too) so windows.h never pulls the old one first.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "logic.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <aclapi.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <wbemidl.h>

#include <atomic>
#include <string>
#include <vector>

namespace portplug {

// ---- the socket tables ---------------------------------------------------------------

// Every TCP row of one family. The owner-PID table is the one that actually
// carries dwState (the owner-module table leaves it zeroed on current
// Windows) and it has the pid, which is all a row needs; the path comes from
// the process API instead of the module info. v6 rows are a different struct
// with 16-byte addresses on both sides.
inline std::vector<SockRow> tcpRows(ADDRESS_FAMILY family)
{
    std::vector<SockRow> rows;
    DWORD size = 0;
    if (::GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0) != ERROR_INSUFFICIENT_BUFFER
        || size == 0)
        return rows;
    std::vector<unsigned char> buffer(size);
    if (::GetExtendedTcpTable(buffer.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR)
        return rows;
    if (family == AF_INET6) {
        const auto* table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buffer.data());
        rows.reserve(table->dwNumEntries);
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const MIB_TCP6ROW_OWNER_PID& entry = table->table[i];
            SockRow row;
            row.proto = "TCP";
            row.v6 = true;
            std::memcpy(row.local, entry.ucLocalAddr, 16);
            row.localPort = ::ntohs(static_cast<u_short>(entry.dwLocalPort));
            std::memcpy(row.remote, entry.ucRemoteAddr, 16);
            row.remotePort = ::ntohs(static_cast<u_short>(entry.dwRemotePort));
            row.state = tcpStateText(entry.dwState);
            row.pid = entry.dwOwningPid;
            rows.push_back(std::move(row));
        }
        return rows;
    }
    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    rows.reserve(table->dwNumEntries);
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_TCPROW_OWNER_PID& entry = table->table[i];
        SockRow row;
        row.proto = "TCP";
        row.v6 = false;
        std::memcpy(row.local, &entry.dwLocalAddr, 4);
        row.localPort = ::ntohs(static_cast<u_short>(entry.dwLocalPort));
        std::memcpy(row.remote, &entry.dwRemoteAddr, 4);
        row.remotePort = ::ntohs(static_cast<u_short>(entry.dwRemotePort));
        row.state = tcpStateText(entry.dwState);
        row.pid = entry.dwOwningPid;
        rows.push_back(std::move(row));
    }
    return rows;
}

// Every UDP endpoint of one family (UDP has no state and no remote side).
// The v6 rows are a different struct — ucLocalAddr[16] instead of a DWORD
// address — so the two families cast to different layouts.
inline std::vector<SockRow> udpRows(ADDRESS_FAMILY family)
{
    std::vector<SockRow> rows;
    DWORD size = 0;
    if (::GetExtendedUdpTable(nullptr, &size, FALSE, family, UDP_TABLE_OWNER_MODULE, 0) != ERROR_INSUFFICIENT_BUFFER
        || size == 0)
        return rows;
    std::vector<unsigned char> buffer(size);
    if (::GetExtendedUdpTable(buffer.data(), &size, FALSE, family, UDP_TABLE_OWNER_MODULE, 0) != NO_ERROR)
        return rows;
    if (family == AF_INET6) {
        const auto* table = reinterpret_cast<const MIB_UDP6TABLE_OWNER_MODULE*>(buffer.data());
        rows.reserve(table->dwNumEntries);
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const MIB_UDP6ROW_OWNER_MODULE& entry = table->table[i];
            SockRow row;
            row.proto = "UDP";
            row.v6 = true;
            std::memcpy(row.local, entry.ucLocalAddr, 16);
            row.localPort = ::ntohs(static_cast<u_short>(entry.dwLocalPort));
            row.pid = entry.dwOwningPid;
            rows.push_back(std::move(row));
        }
        return rows;
    }
    const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_MODULE*>(buffer.data());
    rows.reserve(table->dwNumEntries);
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_UDPROW_OWNER_MODULE& entry = table->table[i];
        SockRow row;
        row.proto = "UDP";
        row.v6 = false;
        std::memcpy(row.local, &entry.dwLocalAddr, 4);
        row.localPort = ::ntohs(static_cast<u_short>(entry.dwLocalPort));
        row.pid = entry.dwOwningPid;
        rows.push_back(std::move(row));
    }
    return rows;
}

// The whole snapshot a query shows: TCP and UDP, v4 and v6, no caching
// (spec §5).
inline std::vector<SockRow> snapshot()
{
    std::vector<SockRow> rows = tcpRows(AF_INET);
    std::vector<SockRow> rest = tcpRows(AF_INET6);
    rows.insert(rows.end(), std::make_move_iterator(rest.begin()), std::make_move_iterator(rest.end()));
    rest = udpRows(AF_INET);
    rows.insert(rows.end(), std::make_move_iterator(rest.begin()), std::make_move_iterator(rest.end()));
    rest = udpRows(AF_INET6);
    rows.insert(rows.end(), std::make_move_iterator(rest.begin()), std::make_move_iterator(rest.end()));
    return rows;
}

// ---- process names and paths -----------------------------------------------------------

// Every running process's exe name, from one Toolhelp32 snapshot (works
// where OpenProcess is denied, e.g. PID 4 and elevated processes).
inline std::map<unsigned long, std::string> processNames()
{
    std::map<unsigned long, std::string> names;
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return names;
    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof entry;
    if (::Process32FirstW(snapshot, &entry)) {
        do {
            names[entry.th32ProcessID] = winshun::utf8(std::wstring(entry.szExeFile));
        } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return names;
}

// The exe's full path, or "" when it cannot be read (spec §5: no error, the
// row just goes without it).
inline std::string processPath(unsigned long pid)
{
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return {};
    wchar_t buffer[1024];
    DWORD length = static_cast<DWORD>(sizeof buffer / sizeof buffer[0]);
    std::string path;
    if (::QueryFullProcessImageNameW(process, 0, buffer, &length))
        path = winshun::utf8(std::wstring(buffer, length));
    ::CloseHandle(process);
    return path;
}

// ---- the kills ---------------------------------------------------------------------------

enum class KillOutcome { Ok, Gone, Denied, Failed };

inline KillOutcome killPid(unsigned long pid)
{
    const HANDLE process = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!process) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_INVALID_PARAMETER)
            return KillOutcome::Gone;
        if (error == ERROR_ACCESS_DENIED)
            return KillOutcome::Denied;
        return KillOutcome::Failed;
    }
    const BOOL killed = ::TerminateProcess(process, 1);
    const DWORD error = ::GetLastError();
    if (killed) {
        // The row should mean "ended" only once it is really gone.
        ::WaitForSingleObject(process, 1000);
    }
    ::CloseHandle(process);
    if (killed)
        return KillOutcome::Ok;
    if (error == ERROR_ACCESS_DENIED)
        return KillOutcome::Denied;
    return KillOutcome::Failed;
}

// Every process as a pid/parent pair, from one Toolhelp32 snapshot: the
// input to both collectTree (killTree) and parentChainText.
inline std::vector<ProcNode> processNodes()
{
    std::vector<ProcNode> procs;
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return procs;
    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof entry;
    if (::Process32FirstW(snapshot, &entry)) {
        do {
            procs.push_back({entry.th32ProcessID, entry.th32ParentProcessID});
        } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return procs;
}

// The pids to end for killTree: `root` and its descendants, from a fresh
// Toolhelp32 snapshot (the tree is read as it is now, at the kill).
inline std::vector<unsigned long> treePids(unsigned long root) { return collectTree(processNodes(), root); }

struct TreeKillResult {
    int killed = 0;
    int denied = 0;
    int failed = 0;
    int total = 0;
};

inline TreeKillResult killTree(unsigned long root)
{
    const std::vector<unsigned long> pids = treePids(root);
    TreeKillResult result;
    result.total = static_cast<int>(pids.size());
    for (const unsigned long pid : pids) {
        switch (killPid(pid)) {
        case KillOutcome::Ok: ++result.killed; break;
        case KillOutcome::Denied: ++result.denied; break;
        case KillOutcome::Gone:
        case KillOutcome::Failed: ++result.failed; break;
        }
    }
    return result;
}

// ---- reveal in Explorer -------------------------------------------------------------------

// Whether the path still exists.
inline bool pathExists(const std::string& path)
{
    return !path.empty() && ::GetFileAttributesW(winshun::wide(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Whether this process can list the folder.
inline bool canListFolder(const std::string& folder)
{
    if (folder.empty())
        return false;
    const HANDLE handle = ::CreateFileW(winshun::wide(folder).c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    ::CloseHandle(handle);
    return true;
}

// Whether `token` may list the folder; nullopt when the DACL cannot be read.
inline std::optional<bool> canListWithToken(const std::string& folder, HANDLE token)
{
    const std::wstring wideFolder = winshun::wide(folder);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    if (::GetNamedSecurityInfoW(const_cast<wchar_t*>(wideFolder.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &dacl, nullptr, &descriptor)
        != ERROR_SUCCESS)
        return std::nullopt;
    GENERIC_MAPPING mapping {FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
    DWORD access = FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES;
    ::MapGenericMask(&access, &mapping);
    PRIVILEGE_SET privileges {};
    DWORD privilegesSize = sizeof privileges;
    DWORD granted = 0;
    BOOL ok = FALSE;
    const BOOL checked = ::AccessCheck(descriptor, token, access, &mapping, &privileges, &privilegesSize, &granted, &ok);
    ::LocalFree(descriptor);
    if (!checked)
        return std::nullopt;
    return ok != FALSE && (granted & FILE_LIST_DIRECTORY) == FILE_LIST_DIRECTORY;
}

// The access token of the shell (explorer.exe), which is where a reveal
// lands. nullptr when the shell cannot be reached.
inline HANDLE shellToken()
{
    const HWND shell = ::GetShellWindow();
    if (!shell)
        return nullptr;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(shell, &pid);
    if (pid == 0 || pid == ::GetCurrentProcessId())
        return nullptr;
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return nullptr;
    HANDLE token = nullptr;
    if (!::OpenProcessToken(process, TOKEN_QUERY, &token))
        token = nullptr;
    ::CloseHandle(process);
    return token;
}

// A standard-user (LUA) token made from our own, or nullptr.
inline HANDLE filteredToken()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &token))
        return nullptr;
    HANDLE filtered = nullptr;
    const BOOL restricted = ::CreateRestrictedToken(token, LUA_TOKEN, 0, nullptr, 0, nullptr, 0, nullptr, &filtered);
    ::CloseHandle(token);
    return restricted ? filtered : nullptr;
}

// Whether this process runs elevated.
inline bool processElevated()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation {};
    DWORD size = sizeof elevation;
    const BOOL got = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size);
    ::CloseHandle(token);
    return got != FALSE && elevation.TokenIsElevated != 0;
}

// Whether the shell can list the folder. The reveal lands in Explorer, which
// runs as the logged-on user even when this plugin was started elevated (a
// SYSTEM service's folder like C:\ProgramData\clash-verge-service\cores is
// readable for an admin but not for the shell), so the answer comes from
// Explorer's own token; a filtered token stands in when the shell is out of
// reach.
inline bool canListFolderAsShell(const std::string& folder)
{
    if (const HANDLE shell = shellToken()) {
        const std::optional<bool> allowed = canListWithToken(folder, shell);
        ::CloseHandle(shell);
        if (allowed)
            return *allowed;
    }
    if (const HANDLE filtered = filteredToken()) {
        const std::optional<bool> allowed = canListWithToken(folder, filtered);
        ::CloseHandle(filtered);
        if (allowed)
            return *allowed;
    }
    // Nobody could read the DACL (a folder that denies even the owner access).
    // Our own access says nothing about Explorer's while we are elevated, so
    // the only safe answer is no.
    return processElevated() ? false : canListFolder(folder);
}

// Select the exe in Explorer. Task Manager has no "select this pid" switch,
// so revealing the file is the closest locate there is; false when Explorer
// refused to start (the path itself is checked by the caller).
inline bool revealPath(const std::string& path)
{
    if (path.empty())
        return false;
    const std::wstring arguments = L"/select,\"" + winshun::wide(path) + L"\"";
    const HINSTANCE result = ::ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr,
        SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

// ---- the WMI command line --------------------------------------------------------------------

// The process's command line via WMI (Win32_Process.CommandLine), or ""
// when there is none (gone, access denied, no COM).
inline std::string commandLineOf(unsigned long pid)
{
    IWbemLocator* locator = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
        reinterpret_cast<void**>(&locator));
    if (FAILED(hr) || !locator)
        return {};
    IWbemServices* services = nullptr;
    const BSTR nameSpace = ::SysAllocString(L"ROOT\\CIMV2");
    hr = locator->ConnectServer(nameSpace, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
    ::SysFreeString(nameSpace);
    locator->Release();
    if (FAILED(hr) || !services)
        return {};
    hr = ::CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHN_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
        RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    if (FAILED(hr)) {
        services->Release();
        return {};
    }
    const std::wstring queryText = L"SELECT CommandLine FROM Win32_Process WHERE ProcessId = " + std::to_wstring(pid);
    const BSTR language = ::SysAllocString(L"WQL");
    const BSTR query = ::SysAllocString(queryText.c_str());
    IEnumWbemClassObject* enumerator = nullptr;
    hr = services->ExecQuery(language, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr,
        &enumerator);
    ::SysFreeString(language);
    ::SysFreeString(query);
    services->Release();
    if (FAILED(hr) || !enumerator)
        return {};
    std::string commandLine;
    IWbemClassObject* object = nullptr;
    ULONG got = 0;
    if (enumerator->Next(WBEM_INFINITE, 1, &object, &got) == S_OK && got == 1 && object) {
        VARIANT value;
        ::VariantInit(&value);
        if (SUCCEEDED(object->Get(L"CommandLine", 0, &value, nullptr, nullptr))) {
            if (value.vt == VT_BSTR && value.bstrVal)
                commandLine = winshun::utf8(std::wstring(value.bstrVal));
            ::VariantClear(&value);
        }
        object->Release();
    }
    enumerator->Release();
    return commandLine;
}

// ---- the UAC kill ------------------------------------------------------------------------------

struct AdminKillResult {
    bool asked = false;   // the UAC dialog came up (or taskkill started)
    bool cancelled = false; // the user said no to UAC
    bool ok = false;
    std::string problem; // set when !ok && !cancelled
};

// One elevated `taskkill /PID n /T /F`. Blocks while the UAC dialog is up,
// so it runs on a worker thread, never the protocol thread (the same rule
// ra9fael.shell's runner.h lives by). `stop`, watched every 200 ms, makes
// the wait abandonable at shutdown.
inline AdminKillResult killAdmin(unsigned long pid, const std::atomic<bool>* stop = nullptr)
{
    AdminKillResult result;
    wchar_t systemDir[MAX_PATH] = {};
    ::GetSystemDirectoryW(systemDir, MAX_PATH);
    const std::wstring file = std::wstring(systemDir) + L"\\taskkill.exe";
    const std::wstring parameters = L"/PID " + std::to_wstring(pid) + L" /T /F";
    SHELLEXECUTEINFOW execute {};
    execute.cbSize = sizeof execute;
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.lpVerb = L"runas";
    execute.lpFile = file.c_str();
    execute.lpParameters = parameters.c_str();
    execute.nShow = SW_HIDE;
    if (!::ShellExecuteExW(&execute)) {
        const DWORD error = ::GetLastError();
        result.cancelled = error == ERROR_CANCELLED;
        if (!result.cancelled)
            result.problem = "ShellExecuteExW error " + std::to_string(error);
        return result;
    }
    result.asked = true;
    if (!execute.hProcess) // started, but its end cannot be waited on
        return result;
    const DWORD start = ::GetTickCount();
    for (;;) {
        const DWORD wait = ::WaitForSingleObject(execute.hProcess, 200);
        if (wait == WAIT_OBJECT_0) {
            DWORD code = 0;
            ::GetExitCodeProcess(execute.hProcess, &code);
            result.ok = code == 0;
            if (!result.ok)
                result.problem = "taskkill exit code " + std::to_string(code);
            break;
        }
        if (stop && stop->load()) {
            result.problem = "abandoned";
            break;
        }
        if (static_cast<int>(::GetTickCount() - start) >= 15000) {
            result.problem = "taskkill did not finish";
            break;
        }
    }
    ::CloseHandle(execute.hProcess);
    return result;
}

} // namespace portplug
