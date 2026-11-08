// main.cpp — ra9fael.port: query who holds a port, end the holder.
// A resident JSON-RPC plugin on stdio; Win32 + CRT only (no Qt, no "ui").
//
// English code/comments per project convention; user-facing strings follow
// the host locale (plugin.chinese()).

#include "native.h"

#include <atomic>
#include <thread>

using namespace winshun;
using namespace portplug;

namespace {

// A hint row (no action, Enter does nothing): an unusable query or an
// unused port.
Item hintItem(const std::string& title, const std::string& subtitle = {})
{
    return Item("port:hint:" + title, title, subtitle).invalid();
}

// "python.exe（PID 1234）", or "PID 1234" when the name is unknown.
std::string whoText(bool zh, const std::string& name, unsigned long pid)
{
    if (name.empty())
        return "PID " + std::to_string(pid);
    return name + (zh ? "（PID " : " (PID ") + std::to_string(pid) + (zh ? "）" : ")");
}

// The folder holding a path, or "" when the path has no separator.
std::string parentFolder(const std::string& path)
{
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// Whether the shell can show the folder. The verdict goes to stderr (which
// the host keeps in plugins.log), so a wrong call can be diagnosed there.
bool shellCanShowFolder(const std::string& folder)
{
    const bool allowed = canListFolderAsShell(folder);
    winshun::debug(std::string("port: reveal ") + (allowed ? "allowed " : "blocked ") + folder);
    return allowed;
}

// The stable id of a table row: protocol, family, endpoints, pid. A remote
// hit (showEstablished) adds its remote endpoint so both sides can coexist.
std::string rowId(const SockRow& row)
{
    std::string id = std::string("port:") + (row.proto == "UDP" ? "udp" : "tcp") + (row.v6 ? ":v6:" : ":v4:")
        + endpoint(row) + ":" + std::to_string(row.pid);
    if (row.proto == "TCP" && row.remotePort != 0)
        id += ":r:" + endpoint(row, true);
    return id;
}

// One row → one item. `confirm` renders the armed (two-stage) variant: the
// title asks, Enter re-fires the same action, red icon. System rows (PID 4
// and 0) override the command-level actions with killAdmin alone; other
// rows inherit them from plugin.json.
Item rowToItem(bool zh, const SockRow& row, const std::string& name, const std::string& path, int portsOfPid,
    bool remoteHit, bool confirm, bool tree, int treeCount)
{
    const bool system = isSystemPid(row.pid);
    Item item;
    item.id = rowId(row);

    if (confirm) {
        item.title = zh ? "⚠ 确认结束 " + whoText(zh, name, row.pid) : "⚠ End " + whoText(zh, name, row.pid) + "?";
        if (tree && treeCount > 1) { // the tree confirm is the louder one
            const std::string target = name.empty() ? whoText(zh, name, row.pid) : name;
            item.title = zh ? "⚠ 确认结束 " + target + " 及其子进程（共 " + std::to_string(treeCount) + " 个进程）？"
                            : "⚠ End " + target + " and its children (" + std::to_string(treeCount) + " processes)?";
        }
        item.subtitle = zh ? "再按 Enter 执行" : "Press Enter to go ahead";
        item.icon = "color:#dc2626";
        item.withData(Json::Object {{"pid", static_cast<long long>(row.pid)},
            {"name", name},
            {"path", path},
            {"confirm", true},
            {"treeCount", treeCount}});
        item.withActions({Action {tree ? "killTree" : "kill", tree ? (zh ? "确认结束进程树" : "Confirm end tree")
                                                                 : (zh ? "确认结束进程" : "Confirm end"),
            "Enter"}});
        return item;
    }

    item.title = rowTitle(row);
    item.subtitle = rowSubtitle(zh, row, name, path, portsOfPid, remoteHit);
    item.icon = system ? "color:#64748b" : row.proto == "UDP" ? "color:#d97706" : "color:#2563eb";
    item.withCopy(endpoint(row));
    item.withData(Json::Object {{"pid", static_cast<long long>(row.pid)},
        {"name", name},
        {"path", path},
        {"line", netstatLine(row)},
        {"confirm", false},
        {"treeCount", 0}});
    if (system) // no path to copy or reveal, and kill needs the UAC detour
        item.withActions({Action {"killAdmin", zh ? "以管理员结束" : "End as admin", ""},
            Action {"copyPid", zh ? "复制 PID" : "Copy PID", ""},
            Action {"copyRow", zh ? "复制整行" : "Copy row", ""},
            Action {"parentChain", zh ? "复制父进程链" : "Copy parent chain", ""}});
    return item;
}

class PortPlugin final : public Plugin {
public:
    std::string name() const override { return "ra9fael.port"; }

    void initialized() override { m_settings = readSettings(settings()); }

    void settingsChanged(const Json& settings) override { m_settings = readSettings(settings); }

    Items query(const Query& q) override { return build(q); }

    Json queryResult(const Query& q) override
    {
        Json::Object result {{"items", itemsJson(build(q))}};
        result["rerun"] = 1.0; // the page re-asks; an ended holder's row disappears
        return Json(std::move(result));
    }

    Effects action(const ActionRequest& a) override
    {
        const bool zh = chinese();
        const Json& data = a.item["data"];
        const unsigned long pid = static_cast<unsigned long>(data["pid"].toInt());
        const std::string name = data["name"].toString();

        if (a.action == "kill") {
            if (!data["confirm"].toBool()) {
                arm(a.item["id"].toString(), false, 0);
                return {effect::keep()};
            }
            standDown();
            switch (killPid(pid)) {
            case KillOutcome::Ok:
                refresh(); // the page re-asks now; the row is gone from it
                return {effect::notify(zh ? "已结束 " + whoText(zh, name, pid) : "Ended " + whoText(zh, name, pid)),
                    effect::keep()};
            case KillOutcome::Gone:
                refresh();
                return {effect::notify(zh ? "进程已经退出了" : "The process is already gone"), effect::keep()};
            case KillOutcome::Denied:
                return {effect::notify(zh ? "没有权限结束它；试试「以管理员结束」" : "Not allowed to end it; try \"End as admin\""),
                    effect::keep()};
            case KillOutcome::Failed:
                return {effect::notify(zh ? "结束失败" : "Could not end it"), effect::keep()};
            }
        }

        if (a.action == "killTree") {
            if (!data["confirm"].toBool()) {
                arm(a.item["id"].toString(), true, static_cast<int>(treePids(pid).size()));
                return {effect::keep()};
            }
            standDown();
            const TreeKillResult r = killTree(pid);
            refresh();
            if (r.denied > 0 && r.killed == 0)
                return {effect::notify(zh ? "没有权限结束它们；试试「以管理员结束」" : "Not allowed to end them; try \"End as admin\""),
                    effect::keep()};
            std::string text;
            if (zh) {
                text = "已结束 " + (name.empty() ? std::to_string(r.killed) + " 个进程" : name + " 及其子进程（共 "
                                                                                                   + std::to_string(r.killed) + " 个）");
                if (r.denied > 0)
                    text += "；" + std::to_string(r.denied) + " 个被拒绝，可试试「以管理员结束」";
            } else {
                text = "Ended " + std::to_string(r.killed) + " process" + (r.killed == 1 ? "" : "es");
                if (r.denied > 0)
                    text += "; " + std::to_string(r.denied) + " refused";
            }
            return {effect::notify(text), effect::keep()};
        }

        if (a.action == "copyCmdline") {
            const std::string commandLine = commandLineOf(pid);
            if (commandLine.empty())
                return {effect::notify(zh ? "读取命令行失败（进程可能已退出）" : "Could not read the command line"),
                    effect::keep()};
            return {effect::copy(commandLine), effect::notify(zh ? "已复制命令行" : "Command line copied")};
        }

        if (a.action == "copyPid") {
            const std::string text = std::to_string(pid);
            return {effect::copy(text), effect::notify(zh ? "已复制 PID " + text : "Copied PID " + text)};
        }

        if (a.action == "copyPath") {
            const std::string path = data["path"].toString();
            if (path.empty())
                return {effect::notify(zh ? "读取不到进程路径" : "The process path is unreadable"), effect::keep()};
            return {effect::copy(path), effect::notify(zh ? "已复制路径" : "Path copied")};
        }

        if (a.action == "copyRow") {
            const std::string line = data["line"].toString();
            if (line.empty())
                return {effect::keep()};
            return {effect::copy(line), effect::notify(zh ? "已复制整行" : "Row copied")};
        }

        if (a.action == "parentChain") {
            const std::string chain = parentChainText(processNodes(), processNames(), pid);
            return {effect::copy(chain), effect::notify(zh ? "已复制父进程链" : "Parent chain copied")};
        }

        if (a.action == "selectExe") {
            const std::string path = data["path"].toString();
            if (path.empty())
                return {effect::notify(zh ? "读取不到进程路径，无法定位" : "The path is unreadable, so it cannot be revealed"),
                    effect::keep()};
            // Explorer runs as the logged-on user, so an admin-only folder
            // (a service's, typically) would only answer with its own
            // "Access is denied" dialog.
            if (!shellCanShowFolder(parentFolder(path)))
                return {effect::copy(path), effect::notify(zh ? "没有访问权限，路径已复制；可在资源管理器中点该文件夹的「继续」授权" : "No access; the path is copied. Open the folder in Explorer and choose Continue to grant access"),
                    effect::keep()};
            if (!pathExists(path))
                return {effect::copy(path), effect::notify(zh ? "文件已不存在，路径已复制" : "The file is gone; its path is copied"),
                    effect::keep()};
            if (!revealPath(path))
                return {effect::notify(zh ? "资源管理器没有打开" : "Explorer did not open"), effect::keep()};
            return {effect::hide()};
        }

        if (a.action == "openFolder") {
            const std::string path = data["path"].toString();
            const std::string folder = parentFolder(path);
            if (folder.empty())
                return {effect::notify(zh ? "读取不到进程路径，没有目录可打开" : "The path is unreadable, so there is no folder to open"),
                    effect::keep()};
            if (!shellCanShowFolder(folder))
                return {effect::copy(path), effect::notify(zh ? "没有访问权限，路径已复制；可在资源管理器中点该文件夹的「继续」授权" : "No access; the path is copied. Open the folder in Explorer and choose Continue to grant access"),
                    effect::keep()};
            return {effect::openPath(folder)};
        }

        if (a.action == "killAdmin") {
            if (m_adminBusy.exchange(true))
                return {effect::notify(zh ? "已有一个提权结束在进行" : "An elevated end is already running"),
                    effect::keep()};
            const bool zhCopy = zh;
            const std::string nameCopy = name;
            m_stopping = false;
            if (m_adminThread.joinable())
                m_adminThread.join();
            m_adminThread = std::thread([this, pid, zhCopy, nameCopy]() {
                const AdminKillResult r = killAdmin(pid, &m_stopping);
                if (m_stopping) { // shutting down: no notifications from here
                    m_adminBusy = false;
                    return;
                }
                m_adminBusy = false;
                if (r.cancelled)
                    notifyUser(zhCopy ? "已取消以管理员结束" : "The elevated end was cancelled");
                else if (r.ok) {
                    notifyUser(zhCopy ? "已结束 " + whoText(zhCopy, nameCopy, pid) : "Ended " + whoText(zhCopy, nameCopy, pid));
                    refresh();
                } else
                    notifyUser(zhCopy ? "以管理员结束失败：" + r.problem : "The elevated end failed: " + r.problem);
            });
            return {effect::notify(zh ? "请在 UAC 窗口确认以管理员结束" : "Confirm the elevation in the UAC window"),
                effect::keep()};
        }

        return {effect::keep()};
    }

    void shuttingDown() override { stopAdminWorker(); }

    ~PortPlugin() override { stopAdminWorker(); }

private:
    // The two-stage confirm: the first Enter arms the row (the page re-asks
    // and renders it armed); only the second Enter ends anything.
    void arm(const std::string& id, bool tree, int treeCount)
    {
        m_pendingId = id;
        m_pendingTree = tree;
        m_pendingTreeCount = treeCount;
        refresh();
    }

    void standDown()
    {
        m_pendingId.clear();
        m_pendingTree = false;
        m_pendingTreeCount = 0;
    }

    void stopAdminWorker()
    {
        m_stopping = true;
        if (m_adminThread.joinable())
            m_adminThread.join();
    }

    Items build(const Query& q)
    {
        const bool zh = chinese();
        if (q.text != m_lastText) { // the user typed: an armed row stands down
            m_lastText = q.text;
            standDown();
        }
        const ParsedQuery parsed = parseQuery(q.text);
        if (!parsed.ok) {
            const std::string shown = winshun::trim(q.text);
            if (parsed.problem == "range")
                return {hintItem(zh ? "端口号要在 1–65535 之间" : "The port must be 1-65535",
                    zh ? "试试 8080" : "Try 8080")};
            return {hintItem(zh ? "「" + shown + "」不是端口号" : "\"" + shown + "\" is not a port number",
                zh ? "试试 8080 或 :8080" : "Try 8080 or :8080")};
        }

        const std::vector<SockRow> rows = snapshot();
        const std::map<unsigned long, std::string> names = processNames();
        const std::map<unsigned long, int> portsByPid = countPortsByPid(rows);

        // The name query: every pid whose exe name contains the needle.
        std::set<unsigned long> namedPids;
        if (!parsed.name.empty())
            for (const auto& [procId, procName] : names)
                if (nameMatches(procName, parsed.name))
                    namedPids.insert(procId);

        std::vector<const SockRow*> picked;
        for (const SockRow& row : rows) {
            if (!m_settings.showSystem && isSystemPid(row.pid))
                continue;
            bool take;
            if (parsed.all)
                take = isListener(row);
            else if (!parsed.name.empty()) // its listeners, plus its live connections
                take = namedPids.count(row.pid) > 0
                    && (isListener(row) || (m_settings.showEstablished && row.state == "ESTABLISHED"));
            else
                take = matchesPort(row, parsed.port, m_settings.showEstablished);
            if (take)
                picked.push_back(&row);
        }
        std::stable_sort(picked.begin(), picked.end(), [](const SockRow* a, const SockRow* b) { return rowLess(*a, *b); });

        Items items;
        for (const SockRow* row : picked) {
            std::string name;
            if (const auto n = names.find(row->pid); n != names.end())
                name = n->second;
            int portsOfPid = 0;
            if (const auto p = portsByPid.find(row->pid); p != portsByPid.end())
                portsOfPid = p->second;
            std::string path;
            if (!isSystemPid(row->pid))
                path = processPath(row->pid);
            const std::string id = rowId(*row);
            const bool confirm = !m_pendingId.empty() && m_pendingId == id;
            const bool remoteHit = parsed.all || !parsed.name.empty() ? false : isRemoteHit(*row, parsed.port);
            items.push_back(rowToItem(zh, *row, name, path, portsOfPid,
                remoteHit, confirm, confirm && m_pendingTree,
                confirm && m_pendingTree ? m_pendingTreeCount : 0));
        }

        // An armed row whose holder is gone stands down.
        if (!m_pendingId.empty()) {
            bool found = false;
            for (const Item& item : items)
                if (item.id == m_pendingId) {
                    found = true;
                    break;
                }
            if (!found)
                standDown();
        }

        if (items.empty()) {
            if (parsed.all)
                return {hintItem(zh ? "没有监听中的端口" : "Nothing is listening")};
            if (!parsed.name.empty()) {
                const std::string shown = winshun::trim(q.text);
                return {hintItem(zh ? "没有名字含「" + shown + "」的进程" : "No process name contains \"" + shown + "\"",
                    zh ? "试试 8080 或 :8080" : "Try 8080 or :8080")};
            }
            return {hintItem(zh ? "端口 " + std::to_string(parsed.port) + " 没有被占用"
                                : "Port " + std::to_string(parsed.port) + " is not in use")};
        }

        if (parsed.all) { // the empty query's cap (a specific port is few rows)
            const int cap = m_settings.maxRows < q.limit ? m_settings.maxRows : q.limit;
            if (static_cast<int>(items.size()) > cap)
                items.resize(static_cast<std::size_t>(cap));
        } else if (!parsed.name.empty() && static_cast<int>(items.size()) > q.limit) {
            items.resize(static_cast<std::size_t>(q.limit)); // one busy process can hold many sockets
        }
        return items;
    }

    Settings m_settings;
    std::string m_lastText;
    std::string m_pendingId;
    bool m_pendingTree = false;
    int m_pendingTreeCount = 0;
    std::atomic<bool> m_stopping {false};
    std::atomic<bool> m_adminBusy {false};
    std::thread m_adminThread;
};

} // namespace

int main()
{
    // WMI (copyCmdline) and ShellExecuteExW (killAdmin) want COM first.
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ::CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr, EOAC_NONE, nullptr);
    int code = 0;
    {
        PortPlugin plugin;
        code = plugin.serve();
    } // joins the admin worker before the plugin is gone
    ::CoUninitialize();
    return code;
}
