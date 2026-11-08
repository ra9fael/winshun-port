// logic.h — ra9fael.port's pure logic: query parsing, address formatting,
// row filtering and ordering, the same-PID note, and killTree's descendant
// collection (with an injected process snapshot). No Win32 calls here, so
// tests/test_port.cpp can run every branch without touching the system.
//
// English code/comments per project convention; user-facing strings follow
// the host locale, chosen by the `zh` flag the plugin passes in.

#pragma once

#include "winshun.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace portplug {

// ---- settings ---------------------------------------------------------------------

struct Settings {
    bool showEstablished = true;
    bool showSystem = true;
    int maxRows = 30; // the empty query's cap; the manifest allows 10-200
};

inline Settings readSettings(const winshun::Json& settings)
{
    Settings st;
    const winshun::Json& established = settings["showEstablished"];
    st.showEstablished = established.isBool() ? established.toBool() : true;
    const winshun::Json& system = settings["showSystem"];
    st.showSystem = system.isBool() ? system.toBool() : true;
    long long rows = settings["maxRows"].toInt(30);
    if (rows < 10)
        rows = 10;
    if (rows > 200)
        rows = 200;
    st.maxRows = static_cast<int>(rows);
    return st;
}

// ---- query parsing ------------------------------------------------------------------

// ASCII lowercasing: process names are ASCII in practice, and non-ASCII bytes
// pass through untouched, so this is enough for a case-insensitive match.
inline std::string lowerAscii(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// Whether a process name contains the needle, ignoring ASCII case:
// "code" matches "Code.exe". An empty needle matches nothing.
inline bool nameMatches(const std::string& processName, const std::string& needle)
{
    if (needle.empty())
        return false;
    return lowerAscii(processName).find(lowerAscii(needle)) != std::string::npos;
}

// Three shapes: empty lists every listener; digits (with or without the
// leading ':') is a port; anything else is a process-name needle (lowercased
// here, matched later against the snapshot's names).
// problem codes: "" when ok, "not-a-number" (a ":" with no digits), "range"
struct ParsedQuery {
    bool ok = false;
    bool all = false; // empty input: every listener
    int port = 0;
    std::string name; // non-empty: process-name lookup
    std::string problem;
};

inline ParsedQuery parseQuery(const std::string& text)
{
    ParsedQuery p;
    const std::string t = winshun::trim(text);
    if (t.empty()) {
        p.ok = true;
        p.all = true;
        return p;
    }
    const bool portIntent = t[0] == ':' || t.find_first_not_of("0123456789") == std::string::npos;
    if (!portIntent) { // "java", "code.exe": a name, not a typo'd port
        p.ok = true;
        p.name = lowerAscii(t);
        return p;
    }
    std::string digits = t;
    if (!digits.empty() && digits[0] == ':') // ":8080" and "8080" are the same
        digits.erase(0, 1);
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
        p.problem = "not-a-number";
        return p;
    }
    if (digits.size() > 9) { // far past 65535 either way; keeps the parse in long long
        p.problem = "range";
        return p;
    }
    long long n = 0;
    for (const char c : digits)
        n = n * 10 + (c - '0');
    if (n < 1 || n > 65535) {
        p.problem = "range";
        return p;
    }
    p.ok = true;
    p.port = static_cast<int>(n);
    return p;
}

// ---- rows ---------------------------------------------------------------------------

// One socket row, family-agnostic: what iphlpapi's tables carry, trimmed to
// what the rows show. `local`/`remote` hold 4 bytes for v4 and 16 for v6.
struct SockRow {
    std::string proto; // "TCP" | "UDP"
    bool v6 = false;
    unsigned char local[16] = {};
    unsigned localPort = 0;
    unsigned char remote[16] = {}; // TCP only
    unsigned remotePort = 0;
    std::string state; // "LISTEN", "ESTABLISHED", ...; UDP has none
    unsigned long pid = 0;
};

// The system entries the manifest's showSystem hint means: PID 4 (System,
// which holds 135/445 and friends) and PID 0.
inline bool isSystemPid(unsigned long pid) { return pid <= 4; }

// MIB_TCP_STATE_* to the netstat-style name; 0 and unknown become "".
inline std::string tcpStateText(unsigned state)
{
    switch (state) {
    case 1: return "CLOSED";
    case 2: return "LISTEN";
    case 3: return "SYN_SENT";
    case 4: return "SYN_RCVD";
    case 5: return "ESTABLISHED";
    case 6: return "FIN_WAIT1";
    case 7: return "FIN_WAIT2";
    case 8: return "CLOSE_WAIT";
    case 9: return "CLOSING";
    case 10: return "LAST_ACK";
    case 11: return "TIME_WAIT";
    case 12: return "DELETE_TCB";
    default: return "";
    }
}

// ---- addresses ------------------------------------------------------------------------

// A v4 address as "0.0.0.0"; b holds 4 bytes, network order.
inline std::string formatIpV4(const unsigned char* b)
{
    std::string out = std::to_string(b[0]);
    out += '.';
    out += std::to_string(b[1]);
    out += '.';
    out += std::to_string(b[2]);
    out += '.';
    out += std::to_string(b[3]);
    return out;
}

// A v6 address in the RFC 5952 shape: lowercase, leading zeros dropped, the
// longest run of two or more zero groups compressed to "::" (the first run
// wins a tie), and ::ffff:0:0/96 shown dotted. b holds 16 bytes.
inline std::string formatIpV6(const unsigned char* b)
{
    unsigned group[8];
    for (int i = 0; i < 8; ++i)
        group[i] = (static_cast<unsigned>(b[2 * i]) << 8) | b[2 * i + 1];

    static const unsigned char v4Mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (std::equal(v4Mapped, v4Mapped + 12, b)) // ::ffff:a.b.c.d
        return "::ffff:" + formatIpV4(b + 12);

    int bestStart = -1;
    int bestLen = 0;
    for (int i = 0; i < 8;) {
        if (group[i] != 0) {
            ++i;
            continue;
        }
        int j = i;
        while (j < 8 && group[j] == 0)
            ++j;
        if (j - i > bestLen) {
            bestLen = j - i;
            bestStart = i;
        }
        i = j;
    }
    if (bestLen < 2)
        bestStart = -1;

    const auto join = [&group](int from, int to) {
        std::string out;
        for (int i = from; i < to; ++i) {
            if (i > from)
                out += ':';
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "%x", group[i]);
            out += buffer;
        }
        return out;
    };
    if (bestStart < 0)
        return join(0, 8);
    const std::string left = join(0, bestStart);
    const std::string right = join(bestStart + bestLen, 8);
    if (left.empty() && right.empty())
        return "::";
    if (left.empty())
        return "::" + right;
    if (right.empty())
        return left + "::";
    return left + "::" + right;
}

// A row's endpoint: "0.0.0.0:8080", v6 in brackets "[::]:8080".
inline std::string endpoint(const SockRow& row, bool remote = false)
{
    const std::string ip = row.v6 ? formatIpV6(remote ? row.remote : row.local)
                                  : formatIpV4(remote ? row.remote : row.local);
    const unsigned port = remote ? row.remotePort : row.localPort;
    return row.v6 ? "[" + ip + "]:" + std::to_string(port) : ip + ":" + std::to_string(port);
}

// ---- filtering and ordering -------------------------------------------------------------

// A row answers to a port query when its local port is it, or (with
// showEstablished) when an ESTABLISHED TCP row reaches the port from the
// other side.
inline bool matchesPort(const SockRow& row, int port, bool showEstablished)
{
    if (row.localPort == static_cast<unsigned>(port))
        return true;
    return showEstablished && row.proto == "TCP" && row.state == "ESTABLISHED"
        && row.remotePort == static_cast<unsigned>(port);
}

// The row matched by its remote side (the "→ 对端" marker).
inline bool isRemoteHit(const SockRow& row, int port) { return row.localPort != static_cast<unsigned>(port); }

// A row with no peer: the remote address is zero and the remote port is 0.
inline bool isRemoteEmpty(const SockRow& row)
{
    const int size = row.v6 ? 16 : 4;
    for (int i = 0; i < size; ++i)
        if (row.remote[i] != 0)
            return false;
    return row.remotePort == 0;
}

// The rows an empty query shows: TCP listeners and bound UDP sockets. Some
// Windows builds report dwState 0 for bound-but-not-connected sockets, so a
// row with no peer counts as a listener even when its state is blank or
// CLOSED.
inline bool isListener(const SockRow& row)
{
    if (row.proto == "UDP")
        return true;
    if (row.state == "LISTEN")
        return true;
    return (row.state.empty() || row.state == "CLOSED") && isRemoteEmpty(row);
}

// The state a row shows: the socket state, with a blank listener state
// rendered as LISTEN so the row does not look stateless.
inline std::string displayState(const SockRow& row)
{
    if (!row.state.empty())
        return row.state;
    return isListener(row) && row.proto == "TCP" ? "LISTEN" : "";
}

// Ascending by port; TCP before UDP; v4 before v6; then by endpoint so the
// order is stable across re-queries.
inline bool rowLess(const SockRow& a, const SockRow& b)
{
    if (a.localPort != b.localPort)
        return a.localPort < b.localPort;
    if (a.proto != b.proto)
        return a.proto < b.proto;
    if (a.v6 != b.v6)
        return !a.v6;
    return endpoint(a) < endpoint(b);
}

// Per PID, how many distinct local ports its rows in the snapshot hold (the
// "同一进程共 N 个端口" note; N is counted over every port, not just the ones
// shown, because kill acts on the whole PID).
inline std::map<unsigned long, int> countPortsByPid(const std::vector<SockRow>& rows)
{
    std::map<unsigned long, std::set<std::string>> per;
    for (const SockRow& row : rows)
        per[row.pid].insert(row.proto + ":" + endpoint(row));
    std::map<unsigned long, int> out;
    for (const auto& [pid, ports] : per)
        out[pid] = static_cast<int>(ports.size());
    return out;
}

// ---- row text ------------------------------------------------------------------------

// The title: "TCP 0.0.0.0:8080", an ESTABLISHED pair as
// "TCP 192.168.1.5:8123 → 93.184.216.34:443", v6 in brackets.
inline std::string rowTitle(const SockRow& row)
{
    std::string out = row.proto + " " + endpoint(row);
    if (row.proto == "TCP" && row.remotePort != 0) {
        out += " → ";
        out += endpoint(row, true);
    }
    return out;
}

// The subtitle's parts, " · " joined: the process ("python.exe" or the PID
// alone when its name is unknown), the PID, the state (TCP only), the path
// (omitted when it could not be read), the same-PID note.
inline std::string rowSubtitle(bool zh, const SockRow& row, const std::string& name, const std::string& path,
    int portsOfPid, bool remoteHit)
{
    std::string out;
    if (remoteHit)
        out += zh ? "对端端口 · " : "remote side · ";
    if (!name.empty())
        out += name + " · ";
    out += "PID " + std::to_string(row.pid);
    const std::string state = displayState(row);
    if (!state.empty())
        out += " · " + state;
    if (!path.empty())
        out += " · " + path;
    if (portsOfPid > 1)
        out += zh ? " · 同一进程共 " + std::to_string(portsOfPid) + " 个端口"
                  : " · same process holds " + std::to_string(portsOfPid) + " ports";
    return out;
}

// The row in `netstat -ano`'s shape, for pasting into a bug report:
// "TCP    0.0.0.0:8080    0.0.0.0:0    LISTENING    1234". UDP has no state
// and netstat prints "*:*" for its remote side.
inline std::string netstatLine(const SockRow& row)
{
    const std::string remote = row.proto == "TCP" ? endpoint(row, true) : "*:*";
    std::string state = displayState(row);
    if (state == "LISTEN") // netstat spells this one differently
        state = "LISTENING";
    std::string out = row.proto + "    " + endpoint(row) + "    " + remote;
    if (!state.empty())
        out += "    " + state;
    out += "    " + std::to_string(row.pid);
    return out;
}

// ---- killTree's collection (snapshot injected) ---------------------------------------------

struct ProcNode {
    unsigned long pid;
    unsigned long parent;
};

// The tree to end for `root`: itself, then its descendants breadth-first (a
// parent before its children, since ending the parent first keeps the
// grandchildren from being re-parented mid-sweep). Parent links come from a
// Toolhelp32 snapshot; a link whose parent is gone, and loops in the links,
// simply contribute nothing.
inline std::vector<unsigned long> collectTree(const std::vector<ProcNode>& procs, unsigned long root)
{
    std::vector<unsigned long> out;
    std::set<unsigned long> seen;
    std::deque<unsigned long> queue;
    seen.insert(root);
    queue.push_back(root);
    while (!queue.empty()) {
        const unsigned long pid = queue.front();
        queue.pop_front();
        out.push_back(pid);
        for (const ProcNode& node : procs) {
            if (node.parent == pid && seen.insert(node.pid).second)
                queue.push_back(node.pid);
        }
    }
    return out;
}

// "python.exe (1234) ← cmd.exe (5678) ← explorer.exe (9012)": who launched
// this process, walking the parent links from one snapshot. Stops at PID 0
// (the root), at a loop, or after `maxDepth` steps; an unknown name shows as
// "PID n".
inline std::string parentChainText(const std::vector<ProcNode>& procs,
    const std::map<unsigned long, std::string>& names, unsigned long pid, int maxDepth = 8)
{
    std::map<unsigned long, unsigned long> parents;
    for (const ProcNode& node : procs)
        parents[node.pid] = node.parent;

    std::string out;
    std::set<unsigned long> seen;
    for (int depth = 0; depth < maxDepth && seen.insert(pid).second; ++depth) {
        if (!out.empty())
            out += " ← ";
        const auto name = names.find(pid);
        out += name == names.end() || name->second.empty() ? "PID " + std::to_string(pid)
                                                          : name->second + " (" + std::to_string(pid) + ")";
        const auto parent = parents.find(pid);
        if (parent == parents.end() || parent->second == 0 || parent->second == pid)
            break;
        pid = parent->second;
    }
    return out;
}

} // namespace portplug
