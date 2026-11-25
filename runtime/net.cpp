// Network (NetDll_*) imports.
//
// No Xbox Live / no network address, but a real socket layer on top of host Winsock: the
// game's RakNet peer (RakPeer::Startup) must be able to create and bind a UDP socket, or the
// game fails startup ("Startup failed. Reason=5", SOCKET_PORT_ALREADY_IN_USE).
//
// Every NetDll_ function takes a leading `caller` argument (r3); the BSD arguments follow.
// Guest sockaddr_in = host layout except sin_family, which is big-endian on the guest.
#include "runtime.h"

#include <mutex>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

constexpr uint32_t XNET_GET_XNADDR_NONE = 0x00000001;
constexpr uint32_t kSocketError = 0xFFFFFFFF;

namespace
{
    std::mutex g_lock;
    std::unordered_map<uint32_t, SOCKET> g_sockets;   // guest socket id -> host socket
    uint32_t g_nextSocket = 0x100;
    thread_local int t_lastError = 0;

    SOCKET HostSocket(uint32_t s)
    {
        std::lock_guard<std::mutex> g(g_lock);
        auto it = g_sockets.find(s);
        return it == g_sockets.end() ? INVALID_SOCKET : it->second;
    }

    void ToHost(uint32_t guest, sockaddr_in& out)
    {
        memset(&out, 0, sizeof(out));
        memcpy(&out, GuestPtr(guest), sizeof(out));
        out.sin_family = Load16(guest);   // big-endian on the guest
    }

    void ToGuest(const sockaddr_in& in, uint32_t guest)
    {
        memcpy(GuestPtr(guest), &in, sizeof(in));
        Store16(guest, in.sin_family);
    }

    uint32_t Fail()
    {
        t_lastError = WSAGetLastError();
        return kSocketError;
    }

    void EnsureWinsock()
    {
        static std::once_flag once;
        std::call_once(once, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
    }
}

IMPORT(NetDll_XNetStartup) { EnsureWinsock(); ctx.r3.u64 = 0; }
IMPORT(NetDll_XNetCleanup) { ctx.r3.u64 = 0; }

// int WSAStartup(caller, u16 version, WSADATA*)
IMPORT(NetDll_WSAStartup)
{
    EnsureWinsock();
    if (uint32_t data = ctx.r5.u32)
    {
        memset(GuestPtr(data), 0, 0x190);
        Store16(data + 0, uint16_t(ctx.r4.u32));   // wVersion
        Store16(data + 2, 0x0202);                 // wHighVersion
    }
    ctx.r3.u64 = 0;
}
IMPORT(NetDll_WSACleanup) { ctx.r3.u64 = 0; }

// DWORD XNetGetTitleXnAddr(caller, XNADDR* addr): no network, but the Ethernet MAC is
// always reported, as on hardware. XNADDR: +0 ina, +4 inaOnline, +8 wPortOnline,
// +10 abEnet[6], +16 abOnline[20]. RakNet derives its GUID from abEnet and fails startup
// with Reason=10 (COULD_NOT_GENERATE_GUID) when it is all zero.
IMPORT(NetDll_XNetGetTitleXnAddr)
{
    if (uint32_t a = ctx.r4.u32)
    {
        memset(GuestPtr(a), 0, 0x24);
        static const uint8_t kMac[6] = { 0x02, 0x50, 0x32, 0x00, 0x00, 0x01 };   // locally administered
        memcpy(GuestPtr(a + 10), kMac, 6);
    }
    ctx.r3.u64 = XNET_GET_XNADDR_NONE;
}

// DWORD XNetGetEthernetLinkStatus(caller): 0 = no link
IMPORT(NetDll_XNetGetEthernetLinkStatus) { ctx.r3.u64 = 0; }

// ---- Sockets ------------------------------------------------------------------------------
// SOCKET socket(caller, af, type, protocol)
// IPPROTO_VDP (254), the Xbox "voice and data protocol", is UDP-based; hosts don't know it,
// so it is mapped to UDP.
IMPORT(NetDll_socket)
{
    EnsureWinsock();
    int af = int(ctx.r4.u32), type = int(ctx.r5.u32), proto = int(ctx.r6.u32);
    int hostProto = proto == 254 ? IPPROTO_UDP : proto;
    SOCKET h = ::socket(af, type, hostProto);
    if (h == INVALID_SOCKET)
    {
        ctx.r3.u64 = Fail();
        Log("[net] socket(af %d, type %d, proto %d) failed: host error %d", af, type, proto, t_lastError);
        return;
    }
    std::lock_guard<std::mutex> g(g_lock);
    uint32_t id = g_nextSocket;
    g_nextSocket += 4;
    g_sockets[id] = h;
    Log("[net] socket(af %u, type %u, proto %u) -> 0x%X", ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, id);
    ctx.r3.u64 = id;
}

// int closesocket(caller, s)
IMPORT(NetDll_closesocket)
{
    SOCKET h;
    {
        std::lock_guard<std::mutex> g(g_lock);
        auto it = g_sockets.find(ctx.r4.u32);
        if (it == g_sockets.end()) { ctx.r3.u64 = kSocketError; return; }
        h = it->second;
        g_sockets.erase(it);
    }
    ctx.r3.u64 = ::closesocket(h) == 0 ? 0 : Fail();
}

// int bind(caller, s, sockaddr*, namelen)
IMPORT(NetDll_bind)
{
    SOCKET h = HostSocket(ctx.r4.u32);
    sockaddr_in a;
    ToHost(ctx.r5.u32, a);
    int r = ::bind(h, (sockaddr*)&a, sizeof(a));
    Log("[net] bind(0x%X, port %u) -> %s", ctx.r4.u32, ntohs(a.sin_port), r == 0 ? "ok" : "failed");
    ctx.r3.u64 = r == 0 ? 0 : Fail();
}

// int getsockname(caller, s, sockaddr* out, int* namelen)
IMPORT(NetDll_getsockname)
{
    SOCKET h = HostSocket(ctx.r4.u32);
    sockaddr_in a{};
    int len = sizeof(a);
    if (::getsockname(h, (sockaddr*)&a, &len) != 0) { ctx.r3.u64 = Fail(); return; }
    ToGuest(a, ctx.r5.u32);
    if (ctx.r6.u32) Store32(ctx.r6.u32, sizeof(a));
    ctx.r3.u64 = 0;
}

// int setsockopt(caller, s, level, optname, const char* optval, optlen)
// Integer options are big-endian on the guest. Xbox-only options are accepted and ignored.
IMPORT(NetDll_setsockopt)
{
    SOCKET h = HostSocket(ctx.r4.u32);
    int level = int(ctx.r5.u32), name = int(ctx.r6.u32);
    uint32_t val = ctx.r7.u32, len = ctx.r8.u32;
    int r;
    if (len == 4)
    {
        int v = int(Load32(val));
        r = ::setsockopt(h, level, name, (const char*)&v, 4);
    }
    else if (len == 1)
    {
        char v = char(Load8(val));
        r = ::setsockopt(h, level, name, &v, 1);
    }
    else
    {
        r = ::setsockopt(h, level, name, (const char*)GuestPtr(val), int(len));
    }
    if (r != 0) Log("[net] setsockopt(level 0x%X, opt 0x%X) not supported on host; ignored", level, name);
    ctx.r3.u64 = 0;
}

// int ioctlsocket(caller, s, cmd, u32* argp)
IMPORT(NetDll_ioctlsocket)
{
    SOCKET h = HostSocket(ctx.r4.u32);
    u_long arg = ctx.r6.u32 ? Load32(ctx.r6.u32) : 0;
    int r = ::ioctlsocket(h, long(ctx.r5.u32), &arg);
    if (r == 0 && ctx.r6.u32) Store32(ctx.r6.u32, uint32_t(arg));
    ctx.r3.u64 = r == 0 ? 0 : Fail();
}

// int sendto(caller, s, buf, len, flags, sockaddr* to, tolen)
IMPORT(NetDll_sendto)
{
    SOCKET h = HostSocket(ctx.r4.u32);
    sockaddr_in to;
    ToHost(ctx.r8.u32, to);
    int r = ::sendto(h, (const char*)GuestPtr(ctx.r5.u32), int(ctx.r6.u32), int(ctx.r7.u32), (sockaddr*)&to, sizeof(to));
    ctx.r3.u64 = r >= 0 ? uint32_t(r) : Fail();
}

// int recvfrom(caller, s, buf, len, flags, sockaddr* from, int* fromlen)
IMPORT(NetDll_recvfrom)
{
    SOCKET h = HostSocket(ctx.r4.u32);
    sockaddr_in from{};
    int fromLen = sizeof(from);
    int r = ::recvfrom(h, (char*)GuestPtr(ctx.r5.u32), int(ctx.r6.u32), int(ctx.r7.u32), (sockaddr*)&from, &fromLen);
    if (r < 0) { ctx.r3.u64 = Fail(); return; }
    if (ctx.r8.u32) ToGuest(from, ctx.r8.u32);
    if (ctx.r9.u32) Store32(ctx.r9.u32, sizeof(from));
    ctx.r3.u64 = uint32_t(r);
}

// int WSASendTo(caller, s, WSABUF* bufs, count, u32* sent, flags, sockaddr* to, tolen,
//               overlapped, completion)   WSABUF = { u32 len; u32 buf; }
IMPORT(NetDll_WSASendTo)
{
    SOCKET h = HostSocket(ctx.r4.u32);
    uint32_t bufs = ctx.r5.u32, count = std::min<uint32_t>(ctx.r6.u32, 16);
    WSABUF hb[16];
    for (uint32_t i = 0; i < count; i++)
    {
        hb[i].len = Load32(bufs + i * 8);
        hb[i].buf = (char*)GuestPtr(Load32(bufs + i * 8 + 4));
    }
    sockaddr_in to;
    ToHost(ctx.r9.u32, to);
    DWORD sent = 0;
    int r = ::WSASendTo(h, hb, count, &sent, ctx.r8.u32, (sockaddr*)&to, sizeof(to), nullptr, nullptr);
    if (ctx.r7.u32) Store32(ctx.r7.u32, sent);
    ctx.r3.u64 = r == 0 ? 0 : Fail();
}

// The following take NO leading `caller` argument.

// u32 inet_addr(const char* cp): network-order address as a big-endian value
IMPORT(NetDll_inet_addr)
{
    std::string s = LoadCString(ctx.r3.u32, 64);
    in_addr a{};
    ctx.r3.u64 = inet_pton(AF_INET, s.c_str(), &a) == 1 ? ntohl(a.s_addr) : 0xFFFFFFFF;
}

// HANDLE WSACreateEvent(void) / BOOL WSACloseEvent(HANDLE)
IMPORT(NetDll_WSACreateEvent) { ctx.r3.u64 = CreateEventHandle(false, false); }
IMPORT(NetDll_WSACloseEvent) { CloseHandle(ctx.r3.u32); ctx.r3.u64 = 1; }

// Xbox-specific address helpers: no secure network, so translation fails.
IMPORT(NetDll_XNetXnAddrToInAddr) { ctx.r3.u64 = WSAEINVAL; }
IMPORT(NetDll_XNetInAddrToString) { ctx.r3.u64 = WSAEINVAL; }

// DNS / QoS lookups fail immediately.
IMPORT(NetDll_XNetDnsLookup) { if (ctx.r6.u32) Store32(ctx.r6.u32, 0); ctx.r3.u64 = WSAENETDOWN; }
IMPORT(NetDll_XNetQosServiceLookup) { ctx.r3.u64 = WSAENETDOWN; }
