// test_port.cpp — ra9fael.port's logic tests (docs/tests.md): query parsing,
// v4/v6 address formatting (IPv4-mapped, "::" compression), state mapping,
// port matching (local/remote, showEstablished), listener set, ordering, the
// same-PID note, row text, killTree's descendant collection, and settings.
// Pure logic.h only — no sockets are touched.

#include "../logic.h"

#include <cstdio>

using namespace portplug;

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                                              \
    do {                                                                                         \
        ++g_checks;                                                                              \
        if (!(cond)) {                                                                           \
            ++g_failed;                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                          \
        }                                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                                                          \
    do {                                                                                                        \
        ++g_checks;                                                                                             \
        if (!((a) == (b))) {                                                                                    \
            ++g_failed;                                                                                         \
            std::printf("FAIL %s:%d: %s == %s (\"%s\" vs \"%s\")\n", __FILE__, __LINE__, #a, #b,                \
                std::string(a).c_str(), std::string(b).c_str());                                                \
        }                                                                                                       \
    } while (0)

// ---- fixtures ---------------------------------------------------------------------------

// A TCP row on 0.0.0.0 (v4) or :: (v6).
static SockRow tcpRow(unsigned localPort, const char* state, unsigned long pid, bool v6 = false,
    unsigned remotePort = 0)
{
    SockRow row;
    row.proto = "TCP";
    row.v6 = v6;
    row.localPort = localPort;
    row.remotePort = remotePort;
    row.state = state;
    row.pid = pid;
    return row;
}

static SockRow udpRow(unsigned localPort, unsigned long pid, bool v6 = false)
{
    SockRow row;
    row.proto = "UDP";
    row.v6 = v6;
    row.localPort = localPort;
    row.pid = pid;
    return row;
}

static void setLocalIp(SockRow& row, unsigned char a, unsigned char b, unsigned char c, unsigned char d)
{
    row.local[0] = a;
    row.local[1] = b;
    row.local[2] = c;
    row.local[3] = d;
}

// ---- 1. query parsing --------------------------------------------------------------------

static void testQueryParsing()
{
    const ParsedQuery plain = parseQuery("8080");
    CHECK(plain.ok && !plain.all && plain.port == 8080);

    const ParsedQuery colon = parseQuery(":8080");
    CHECK(colon.ok && !colon.all && colon.port == 8080);

    const ParsedQuery empty = parseQuery("");
    CHECK(empty.ok && empty.all);

    const ParsedQuery spaces = parseQuery("   ");
    CHECK(spaces.ok && spaces.all);

    const ParsedQuery zero = parseQuery("0");
    CHECK(!zero.ok && zero.problem == "range");

    const ParsedQuery tooBig = parseQuery("65536");
    CHECK(!tooBig.ok && tooBig.problem == "range");

    const ParsedQuery huge = parseQuery("1234567890");
    CHECK(!huge.ok && huge.problem == "range");

    const ParsedQuery letters = parseQuery("abc"); // a process name, not a typo
    CHECK(letters.ok && !letters.all && letters.port == 0 && letters.name == "abc");

    const ParsedQuery mixedCase = parseQuery("  Java.EXE ");
    CHECK(mixedCase.ok && mixedCase.name == "java.exe");

    const ParsedQuery negative = parseQuery("-5"); // name-intent, matches nothing
    CHECK(negative.ok && negative.name == "-5");

    const ParsedQuery bareColon = parseQuery(":");
    CHECK(!bareColon.ok && bareColon.problem == "not-a-number");

    const ParsedQuery colonName = parseQuery(":abc"); // the ':' stays port-intent
    CHECK(!colonName.ok && colonName.problem == "not-a-number");

    const ParsedQuery spaced = parseQuery(" 8 0 "); // trimmed to "8 0"
    CHECK(spaced.ok && spaced.name == "8 0");

    const ParsedQuery leadingZeros = parseQuery("007");
    CHECK(leadingZeros.ok && leadingZeros.port == 7);
}

// ---- 2. addresses --------------------------------------------------------------------------

static void testAddresses()
{
    const unsigned char zeros4[4] = {0, 0, 0, 0};
    CHECK_EQ(formatIpV4(zeros4), "0.0.0.0");

    const unsigned char loopback[4] = {127, 0, 0, 1};
    CHECK_EQ(formatIpV4(loopback), "127.0.0.1");

    const unsigned char zeros6[16] = {};
    CHECK_EQ(formatIpV6(zeros6), "::");

    const unsigned char db8[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    CHECK_EQ(formatIpV6(db8), "2001:db8::1");

    const unsigned char fe80[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    CHECK_EQ(formatIpV6(fe80), "fe80::1");

    // The longest zero run wins, wherever it sits.
    const unsigned char mixed[16] = {0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1}; // 1:0:0:1:0:0:0:1
    CHECK_EQ(formatIpV6(mixed), "1:0:0:1::1");

    // Equal-length runs: the first one compresses (RFC 5952). 0:1:0:0:1:0:0:1
    // has two runs of two zero groups; the earlier one goes.
    const unsigned char tie[16] = {0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1};
    CHECK_EQ(formatIpV6(tie), "0:1::1:0:0:1");

    const unsigned char mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 168, 1, 5};
    CHECK_EQ(formatIpV6(mapped), "::ffff:192.168.1.5");

    SockRow v4 = tcpRow(8080, "LISTEN", 1);
    CHECK_EQ(endpoint(v4), "0.0.0.0:8080");

    SockRow v6 = udpRow(53, 1, true);
    CHECK_EQ(endpoint(v6), "[::]:53");
}

// ---- 3. states -----------------------------------------------------------------------------

static void testStates()
{
    CHECK_EQ(tcpStateText(2), "LISTEN");
    CHECK_EQ(tcpStateText(5), "ESTABLISHED");
    CHECK_EQ(tcpStateText(11), "TIME_WAIT");
    CHECK_EQ(tcpStateText(1), "CLOSED");
    CHECK_EQ(tcpStateText(0), "");
    CHECK_EQ(tcpStateText(13), "");
}

// ---- 4. matching ---------------------------------------------------------------------------

static void testMatching()
{
    const SockRow local = tcpRow(8080, "LISTEN", 10);
    CHECK(matchesPort(local, 8080, false));
    CHECK(matchesPort(local, 8080, true));

    const SockRow pair = tcpRow(51000, "ESTABLISHED", 10, false, 443);
    CHECK(!matchesPort(pair, 443, false)); // remote side needs showEstablished
    CHECK(matchesPort(pair, 443, true));
    CHECK(matchesPort(pair, 51000, true)); // its own local side answers too

    const SockRow udp = udpRow(5353, 10);
    CHECK(matchesPort(udp, 5353, false));
    CHECK(!matchesPort(udp, 53, false));

    CHECK(isRemoteHit(pair, 443));
    CHECK(!isRemoteHit(local, 8080));

    CHECK(isListener(local));
    CHECK(!isListener(pair));
    CHECK(isListener(udp));

    // Some Windows builds report dwState 0 for bound sockets: a row with no
    // peer is a listener anyway, a row with a peer is not.
    const SockRow blank = tcpRow(9000, "", 7);
    CHECK(isListener(blank));
    CHECK_EQ(displayState(blank), "LISTEN");
    const SockRow blankPeer = tcpRow(51000, "", 7, false, 443);
    CHECK(!isListener(blankPeer));
    CHECK_EQ(displayState(blankPeer), "");
    CHECK(isListener(tcpRow(9001, "CLOSED", 7))); // closed, no peer: still bound
    CHECK_EQ(displayState(tcpRow(9001, "CLOSED", 7)), "CLOSED");

    CHECK(isRemoteEmpty(local));
    CHECK(!isRemoteEmpty(pair));
    const SockRow udp6 = udpRow(53, 7, true);
    CHECK(isRemoteEmpty(udp6)); // all 16 address bytes zero
}

// ---- 5. ordering -----------------------------------------------------------------------------

static void testOrdering()
{
    CHECK(rowLess(tcpRow(80, "LISTEN", 1), tcpRow(443, "LISTEN", 1)));
    CHECK(rowLess(tcpRow(80, "LISTEN", 1), udpRow(80, 1))); // TCP before UDP
    CHECK(rowLess(tcpRow(80, "LISTEN", 1), tcpRow(80, "LISTEN", 1, true))); // v4 before v6

    std::vector<SockRow> rows {
        udpRow(5353, 3), tcpRow(443, "LISTEN", 2), udpRow(80, 4), tcpRow(80, "LISTEN", 1)};
    std::stable_sort(rows.begin(), rows.end(), [](const SockRow& a, const SockRow& b) { return rowLess(a, b); });
    CHECK(rows[0].proto == "TCP" && rows[0].localPort == 80);
    CHECK(rows[1].proto == "UDP" && rows[1].localPort == 80);
    CHECK(rows[2].localPort == 443);
    CHECK(rows[3].localPort == 5353);
}

// ---- 6. the same-PID note ----------------------------------------------------------------------

static void testAggregation()
{
    std::vector<SockRow> rows;
    SockRow a = tcpRow(80, "LISTEN", 100);
    SockRow b = udpRow(53, 100);
    SockRow c = tcpRow(80, "LISTEN", 100, true); // the same port, v6 side
    SockRow d = tcpRow(8080, "LISTEN", 200);
    rows = {a, b, c, d};
    const std::map<unsigned long, int> counts = countPortsByPid(rows);
    CHECK(counts.at(100) == 3);
    CHECK(counts.at(200) == 1);

    rows = {a, a}; // the same endpoint twice counts once
    CHECK(countPortsByPid(rows).at(100) == 1);

    CHECK_EQ(rowSubtitle(true, a, "python.exe", "", 2, false),
        "python.exe · PID 100 · LISTEN · 同一进程共 2 个端口");
    CHECK_EQ(rowSubtitle(false, a, "python.exe", "", 2, false),
        "python.exe · PID 100 · LISTEN · same process holds 2 ports");
    CHECK_EQ(rowSubtitle(true, a, "python.exe", "", 1, false), "python.exe · PID 100 · LISTEN");
}

// ---- 7. row text ---------------------------------------------------------------------------------

static void testRowText()
{
    const SockRow listen = tcpRow(8080, "LISTEN", 123);
    CHECK_EQ(rowTitle(listen), "TCP 0.0.0.0:8080");

    SockRow pair = tcpRow(51000, "ESTABLISHED", 9, false, 443);
    setLocalIp(pair, 1, 2, 3, 4);
    pair.remote[0] = 5;
    pair.remote[1] = 6;
    pair.remote[2] = 7;
    pair.remote[3] = 8;
    CHECK_EQ(rowTitle(pair), "TCP 1.2.3.4:51000 → 5.6.7.8:443");

    const SockRow udp = udpRow(53, 9, true);
    CHECK_EQ(rowTitle(udp), "UDP [::]:53");

    CHECK_EQ(rowSubtitle(true, listen, "python.exe", "D:\\x\\p.exe", 1, false),
        "python.exe · PID 123 · LISTEN · D:\\x\\p.exe");
    CHECK_EQ(rowSubtitle(true, udp, "python.exe", "", 1, false), "python.exe · PID 9"); // UDP: no state part
    CHECK_EQ(rowSubtitle(false, listen, "", "", 1, false), "PID 123 · LISTEN"); // unknown name: the PID alone
    CHECK_EQ(rowSubtitle(true, listen, "", "", 1, true), "对端端口 · PID 123 · LISTEN");
    CHECK_EQ(rowSubtitle(false, listen, "", "", 1, true), "remote side · PID 123 · LISTEN");
}

// ---- 8. process-name lookup ------------------------------------------------------------------------

static void testNameMatching()
{
    CHECK(nameMatches("Code.exe", "code"));
    CHECK(nameMatches("Code.exe", "CODE"));
    CHECK(nameMatches("Code.exe", "de.ex"));
    CHECK(!nameMatches("Code.exe", "java"));
    CHECK(!nameMatches("Code.exe", "")); // an empty needle matches nothing
    CHECK(nameMatches("PID", "id")); // any substring, not just a prefix
    CHECK_EQ(lowerAscii("AbC.exe"), "abc.exe");
    CHECK_EQ(lowerAscii("服务.exe"), "服务.exe"); // non-ASCII bytes pass through
}

// ---- 9. the netstat-shaped line ----------------------------------------------------------------------

static void testNetstatLine()
{
    const SockRow listen = tcpRow(8080, "LISTEN", 123);
    CHECK_EQ(netstatLine(listen), "TCP    0.0.0.0:8080    0.0.0.0:0    LISTENING    123");

    SockRow pair = tcpRow(51000, "ESTABLISHED", 9, false, 443);
    setLocalIp(pair, 1, 2, 3, 4);
    pair.remote[0] = 5;
    pair.remote[1] = 6;
    pair.remote[2] = 7;
    pair.remote[3] = 8;
    CHECK_EQ(netstatLine(pair), "TCP    1.2.3.4:51000    5.6.7.8:443    ESTABLISHED    9");

    CHECK_EQ(netstatLine(udpRow(53, 9, true)), "UDP    [::]:53    *:*    9"); // no state column

    // A listener whose state the OS left blank still reads as listening.
    CHECK_EQ(netstatLine(tcpRow(9000, "", 7)), "TCP    0.0.0.0:9000    0.0.0.0:0    LISTENING    7");
    CHECK_EQ(rowSubtitle(true, tcpRow(9000, "", 7), "x.exe", "", 1, false), "x.exe · PID 7 · LISTEN");
}

// ---- 10. the parent chain ------------------------------------------------------------------------------

static void testParentChain()
{
    const std::vector<ProcNode> chain {{1, 0}, {2, 1}, {3, 2}};
    const std::map<unsigned long, std::string> names {
        {1, "explorer.exe"}, {2, "cmd.exe"}, {3, "python.exe"}};
    CHECK_EQ(parentChainText(chain, names, 3), "python.exe (3) ← cmd.exe (2) ← explorer.exe (1)");

    CHECK_EQ(parentChainText(chain, names, 9), "PID 9"); // not in the snapshot

    CHECK_EQ(parentChainText(chain, names, 3, 2), "python.exe (3) ← cmd.exe (2)"); // depth cap

    const std::vector<ProcNode> cycle {{1, 2}, {2, 1}};
    CHECK_EQ(parentChainText(cycle, {}, 1), "PID 1 ← PID 2"); // a loop ends instead of spinning

    const std::vector<ProcNode> self {{5, 5}};
    CHECK_EQ(parentChainText(self, {}, 5), "PID 5"); // a self-parent ends too

    const std::map<unsigned long, std::string> partly {{3, ""}};
    CHECK_EQ(parentChainText(chain, partly, 3), "PID 3 ← PID 2 ← PID 1"); // an empty name falls back
}

// ---- 11. killTree's collection ---------------------------------------------------------------------

static void testCollectTree()
{
    // A chain: 1 → 2 → 3, all in parent-before-child order.
    const std::vector<ProcNode> chain {{1, 0}, {2, 1}, {3, 2}};
    const std::vector<unsigned long> chainOut = collectTree(chain, 1);
    CHECK(chainOut.size() == 3 && chainOut[0] == 1 && chainOut[1] == 2 && chainOut[2] == 3);

    // Two children, each with a grandchild.
    const std::vector<ProcNode> wide {{1, 0}, {2, 1}, {3, 1}, {4, 2}, {5, 3}};
    const std::vector<unsigned long> wideOut = collectTree(wide, 1);
    CHECK(wideOut.size() == 5 && wideOut[0] == 1 && wideOut[1] == 2 && wideOut[2] == 3 && wideOut[3] == 4
        && wideOut[4] == 5);

    // A root absent from the snapshot still returns itself; a node whose
    // parent link points outside the tree contributes nothing.
    const std::vector<ProcNode> orphan {{7, 100}};
    const std::vector<unsigned long> orphanOut = collectTree(orphan, 99);
    CHECK(orphanOut.size() == 1 && orphanOut[0] == 99);

    // A parent link loop ends instead of spinning.
    const std::vector<ProcNode> cycle {{1, 2}, {2, 1}};
    CHECK(collectTree(cycle, 1).size() == 2);

    // A self-parented node ends too.
    const std::vector<ProcNode> self {{5, 5}};
    const std::vector<unsigned long> selfOut = collectTree(self, 5);
    CHECK(selfOut.size() == 1 && selfOut[0] == 5);

    // A sibling is not a descendant.
    const std::vector<ProcNode> siblings {{2, 1}, {3, 1}};
    const std::vector<unsigned long> siblingsOut = collectTree(siblings, 2);
    CHECK(siblingsOut.size() == 1 && siblingsOut[0] == 2);
}

// ---- 9. settings -----------------------------------------------------------------------------------

static void testSettings()
{
    const Settings defaults = readSettings(winshun::Json::Object {});
    CHECK(defaults.showEstablished && defaults.showSystem && defaults.maxRows == 30);

    const winshun::Json low(winshun::Json::Object {{"maxRows", 5}});
    CHECK(readSettings(low).maxRows == 10);
    const winshun::Json high(winshun::Json::Object {{"maxRows", 500}});
    CHECK(readSettings(high).maxRows == 200);

    const winshun::Json off(winshun::Json::Object {{"showEstablished", false}, {"showSystem", false}});
    const Settings parsed = readSettings(off);
    CHECK(!parsed.showEstablished && !parsed.showSystem);
}

int main()
{
    testQueryParsing();
    testAddresses();
    testStates();
    testMatching();
    testOrdering();
    testAggregation();
    testRowText();
    testNameMatching();
    testNetstatLine();
    testParentChain();
    testCollectTree();
    testSettings();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
