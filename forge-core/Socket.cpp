// Win32/Winsock implementation of Socket — see Socket.md. Cannot be
// compiled in the sandbox this was written in (no Windows SDK / working
// MinGW cross compiler available there); verified by manual review plus
// a hand-written mock <winsock2.h> compile+link pass only. Needs a real
// `mach build` (or a standalone Visual Studio smoke test, see
// SocketSmokeTest.cpp) to go from "implemented" to "confirmed working"
// — flagged explicitly per AGENTS.md's "Be Honest".

#if !defined(_WIN32)
#error "forge::core::Socket: no backend implemented for this platform (Win32/Winsock only, matching forge::core::File's own precedent)."
#endif

#include "Socket.h"

#include <climits>
#include <cstring>
#include <utility>

// <windows.h> pulls in the legacy <winsock.h> by default unless
// WIN32_LEAN_AND_MEAN is defined first, and having both <winsock.h> and
// <winsock2.h> included together (in the wrong order) is a classic
// redefinition-error trap — so WIN32_LEAN_AND_MEAN is defined here and
// <winsock2.h>/<ws2tcpip.h> are included before <windows.h>, matching
// the include order platform/Win32Error.cpp also follows.
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "platform/Win32Error.h"

// ws2_32.lib is required to link WSAStartup/socket/connect/etc. mach's
// moz.build carries this as a linker input (see moz.build's OS_LIBS
// entry added alongside this file) rather than relying on this #pragma,
// but the pragma is kept too as a standard MSVC belt-and-suspenders
// convenience for anyone building this file in isolation (e.g. a
// standalone Visual Studio smoke test project, matching
// SocketSmokeTest.cpp's own precedent).
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#endif

namespace forge::core
{

namespace
{

using platform::TranslateWin32Error;
using platform::TranslateWinsockError;

//==============================================================================
// One-time Winsock initialization.
//==============================================================================

/// RAII wrapper around WSAStartup/WSACleanup. A single instance is
/// created lazily via a function-local static (thread-safe init
/// guaranteed by C++11, the same pattern memory::GetDefaultAllocator()
/// already establishes in this codebase) so every Socket entry point
/// can just call EnsureWinsockInitialized() without callers ever having
/// to think about Winsock's own startup/shutdown protocol.
class WinsockInitializer final
{
public:

    WinsockInitializer() noexcept
    {
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        succeeded_ = (result == 0);
    }

    WinsockInitializer(const WinsockInitializer&) = delete;
    WinsockInitializer& operator=(const WinsockInitializer&) = delete;

    ~WinsockInitializer() noexcept
    {
        if (succeeded_)
        {
            WSACleanup();
        }
    }

    [[nodiscard]]
    bool Succeeded() const noexcept
    {
        return succeeded_;
    }

private:

    bool succeeded_{ false };
};

/// Returns whether Winsock is ready to use. The WinsockInitializer
/// itself lives for the lifetime of the process once first constructed
/// — deliberately never torn down early, since forge-core has no
/// process-shutdown hook to call a matching "last Socket use" moment,
/// and WSACleanup() running at static-destruction time (after main
/// returns) is standard, well-defined Winsock usage.
[[nodiscard]]
bool EnsureWinsockInitialized() noexcept
{
    static const WinsockInitializer initializer;
    return initializer.Succeeded();
}

//==============================================================================
// SOCKET <-> void* helpers.
//==============================================================================

// SOCKET is UINT_PTR — pointer-sized on every Windows target this
// project builds for — so this reinterpret_cast is a legal integral<->
// pointer round trip, not a type-punning hazard. Kept as tiny named
// helpers rather than repeating the casts inline everywhere, matching
// how File.cpp centralizes its own HANDLE<->void* casts implicitly via
// static_cast at each call site (Socket's cast needs reinterpret_cast
// instead, since SOCKET is an integer type, not a pointer type, so it
// gets these two named wrappers to keep every call site readable).
[[nodiscard]]
SOCKET NativeSocket(
    void* handle) noexcept
{
    return static_cast<SOCKET>(reinterpret_cast<UINT_PTR>(handle));
}

[[nodiscard]]
void* ToHandle(
    SOCKET socket) noexcept
{
    return reinterpret_cast<void*>(static_cast<UINT_PTR>(socket));
}

//==============================================================================
// Endpoint -> sockaddr_storage.
//==============================================================================

/// Fills `out` with the sockaddr_in/sockaddr_in6 representation of
/// `endpoint` and returns the addrlen a caller should pass to
/// bind()/connect(). Never fails — an Endpoint's IpAddress is always
/// either V4 or V6, and Bytes() always returns the matching fixed size,
/// so there is no malformed-input case to report here (see
/// IpAddress.md's own "constructors never fail" precedent).
[[nodiscard]]
int FillSockaddr(
    const Endpoint& endpoint,
    sockaddr_storage& out) noexcept
{
    std::memset(&out, 0, sizeof(out));

    const IpAddress& address = endpoint.Address();
    const Span<const u8> bytes = address.Bytes();

    if (address.IsV4())
    {
        sockaddr_in* v4 = reinterpret_cast<sockaddr_in*>(&out);
        v4->sin_family = AF_INET;
        v4->sin_port = htons(endpoint.Port());
        std::memcpy(&v4->sin_addr, bytes.Data(), bytes.Size());
        return static_cast<int>(sizeof(sockaddr_in));
    }

    sockaddr_in6* v6 = reinterpret_cast<sockaddr_in6*>(&out);
    v6->sin6_family = AF_INET6;
    v6->sin6_port = htons(endpoint.Port());
    std::memcpy(&v6->sin6_addr, bytes.Data(), bytes.Size());
    return static_cast<int>(sizeof(sockaddr_in6));
}

/// Creates a TCP socket that child processes can never inherit (Phase
/// 7.9). socket() creates an inheritable handle by default; once Forge
/// spawns child processes, an inheritable listening or connected socket
/// would leak into them. WSA_FLAG_OVERLAPPED matches what socket() sets.
SOCKET CreateNonInheritableSocket(
    int family) noexcept
{
    return WSASocketW(
        family,
        SOCK_STREAM,
        IPPROTO_TCP,
        nullptr,
        0,
        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}

/// Converts a peer sockaddr into an Endpoint, unwrapping IPv4-mapped
/// IPv6 addresses. Returns false for an unexpected address family.
bool EndpointFromSockaddr(
    const sockaddr_storage& storage,
    Endpoint& out) noexcept
{
    if (storage.ss_family == AF_INET)
    {
        const sockaddr_in* v4 = reinterpret_cast<const sockaddr_in*>(&storage);
        const u8* b = reinterpret_cast<const u8*>(&v4->sin_addr);
        out = Endpoint(IpAddress::V4(b[0], b[1], b[2], b[3]), ntohs(v4->sin_port));
        return true;
    }

    if (storage.ss_family == AF_INET6)
    {
        const sockaddr_in6* v6 = reinterpret_cast<const sockaddr_in6*>(&storage);
        const u8* b = reinterpret_cast<const u8*>(&v6->sin6_addr);
        const u16 port = ntohs(v6->sin6_port);

        bool mapped = b[10] == 0xFF && b[11] == 0xFF;

        for (int i = 0; i < 10 && mapped; ++i)
        {
            mapped = b[i] == 0;
        }

        if (mapped)
        {
            out = Endpoint(IpAddress::V4(b[12], b[13], b[14], b[15]), port);
        }
        else
        {
            out = Endpoint(IpAddress::V6(Span<const u8>(b, 16)), port);
        }

        return true;
    }

    return false;
}

} // namespace

//==============================================================================
// Construction
//==============================================================================

Socket::Socket() noexcept
    :
    handle_(nullptr),
    isListening_(false)
{
}

Socket::Socket(
    void* handle) noexcept
    :
    handle_(handle),
    isListening_(false)
{
}

Socket::Socket(
    Socket&& other) noexcept
    :
    handle_(other.handle_),
    isListening_(other.isListening_)
{
    other.handle_ = nullptr;
    other.isListening_ = false;
}

Socket& Socket::operator=(
    Socket&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    Close();

    handle_ = other.handle_;
    isListening_ = other.isListening_;

    other.handle_ = nullptr;
    other.isListening_ = false;

    return *this;
}

Socket::~Socket() noexcept
{
    Close();
}

//==============================================================================
// Connect / Listen / Accept
//==============================================================================

Result<Socket> Socket::Connect(
    const Endpoint& remote)
{
    if (!EnsureWinsockInitialized())
    {
        return Result<Socket>(Failure{ Error(ErrorCode::PlatformError) });
    }

    const int family = remote.Address().IsV4() ? AF_INET : AF_INET6;

    const SOCKET native = CreateNonInheritableSocket(family);

    if (native == INVALID_SOCKET)
    {
        return Result<Socket>(Failure{ TranslateWinsockError(WSAGetLastError()) });
    }

    sockaddr_storage address{};
    const int addressLength = FillSockaddr(remote, address);

    if (connect(native, reinterpret_cast<const sockaddr*>(&address), addressLength) == SOCKET_ERROR)
    {
        const Error error = TranslateWinsockError(WSAGetLastError());
        closesocket(native);
        return Result<Socket>(Failure{ error });
    }

    return Result<Socket>(Socket(ToHandle(native)));
}

Result<Socket> Socket::Listen(
    const Endpoint& local,
    int backlog)
{
    if (!EnsureWinsockInitialized())
    {
        return Result<Socket>(Failure{ Error(ErrorCode::PlatformError) });
    }

    const int family = local.Address().IsV4() ? AF_INET : AF_INET6;

    const SOCKET native = CreateNonInheritableSocket(family);

    if (native == INVALID_SOCKET)
    {
        return Result<Socket>(Failure{ TranslateWinsockError(WSAGetLastError()) });
    }

    // Phase 7.9: this used to set SO_REUSEADDR. On Windows that option
    // does not mean what it means on POSIX: it lets ANOTHER socket, even
    // in another process, bind the same port and steal connections
    // (study 09; libuv deliberately never sets it). SO_EXCLUSIVEADDRUSE
    // forbids that instead. Windows' TIME_WAIT handling already lets a
    // listener restart on the same port in the common case.
    const int exclusive = 1;

    if (setsockopt(
            native,
            SOL_SOCKET,
            SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive),
            static_cast<int>(sizeof(exclusive))) == SOCKET_ERROR)
    {
        const Error error = TranslateWinsockError(WSAGetLastError());
        closesocket(native);
        return Result<Socket>(Failure{ error });
    }

    sockaddr_storage address{};
    const int addressLength = FillSockaddr(local, address);

    if (bind(native, reinterpret_cast<const sockaddr*>(&address), addressLength) == SOCKET_ERROR)
    {
        const Error error = TranslateWinsockError(WSAGetLastError());
        closesocket(native);
        return Result<Socket>(Failure{ error });
    }

    // Clamp to SOMAXCONN rather than passing an oversized backlog
    // straight through — Winsock already does this clamping internally,
    // but doing it here too keeps this call site self-documenting.
    const int clampedBacklog = (backlog > SOMAXCONN) ? SOMAXCONN : backlog;

    if (listen(native, clampedBacklog) == SOCKET_ERROR)
    {
        const Error error = TranslateWinsockError(WSAGetLastError());
        closesocket(native);
        return Result<Socket>(Failure{ error });
    }

    Socket result(ToHandle(native));
    result.isListening_ = true;
    return Result<Socket>(std::move(result));
}

Result<Socket> Socket::Accept()
{
    Endpoint ignored;
    return Accept(ignored);
}

Result<Socket> Socket::Accept(
    Endpoint& peer)
{
    if (handle_ == nullptr || !isListening_)
    {
        return Result<Socket>(Failure{ Error(ErrorCode::InvalidOperation) });
    }

    sockaddr_storage address{};
    int addressLength = static_cast<int>(sizeof(address));

    const SOCKET accepted = accept(
        NativeSocket(handle_),
        reinterpret_cast<sockaddr*>(&address),
        &addressLength);

    if (accepted == INVALID_SOCKET)
    {
        return Result<Socket>(Failure{ TranslateWinsockError(WSAGetLastError()) });
    }

    // Wrap first, so every failure path below closes the socket.
    Socket result(ToHandle(accepted));

    // The listening socket is already non-inheritable, but clear the flag
    // on the accepted handle explicitly rather than rely on inheritance of
    // that property (Phase 7.9).
    if (!SetHandleInformation(reinterpret_cast<HANDLE>(accepted), HANDLE_FLAG_INHERIT, 0))
    {
        return Result<Socket>(Failure{ TranslateWin32Error(GetLastError()) });
    }

    if (!EndpointFromSockaddr(address, peer))
    {
        return Result<Socket>(Failure{ Error(ErrorCode::NotSupported) });
    }

    return Result<Socket>(std::move(result));
}

//==============================================================================
// IsOpen / Send / Receive / Close
//==============================================================================

bool Socket::IsOpen() const noexcept
{
    return handle_ != nullptr;
}

Result<Socket::SizeType> Socket::Send(
    Span<const u8> buffer)
{
    if (handle_ == nullptr || isListening_)
    {
        return Result<SizeType>(Failure{ Error(ErrorCode::InvalidOperation) });
    }

    if (buffer.Empty())
    {
        return Result<SizeType>(SizeType{ 0 });
    }

    // send()'s length parameter is a 32-bit `int` — guard against a
    // caller-supplied buffer larger than that rather than letting an
    // implicit narrowing cast silently truncate it into a smaller,
    // wrong send length.
    if (buffer.Size() > static_cast<Size>(INT_MAX))
    {
        return Result<SizeType>(Failure{ Error(ErrorCode::InvalidArgument) });
    }

    const int sent = send(
        NativeSocket(handle_),
        reinterpret_cast<const char*>(buffer.Data()),
        static_cast<int>(buffer.Size()),
        0);

    if (sent == SOCKET_ERROR)
    {
        return Result<SizeType>(Failure{ TranslateWinsockError(WSAGetLastError()) });
    }

    return Result<SizeType>(static_cast<SizeType>(sent));
}

Result<Socket::SizeType> Socket::Receive(
    Span<u8> buffer)
{
    if (handle_ == nullptr || isListening_)
    {
        return Result<SizeType>(Failure{ Error(ErrorCode::InvalidOperation) });
    }

    if (buffer.Empty())
    {
        return Result<SizeType>(SizeType{ 0 });
    }

    if (buffer.Size() > static_cast<Size>(INT_MAX))
    {
        return Result<SizeType>(Failure{ Error(ErrorCode::InvalidArgument) });
    }

    const int received = recv(
        NativeSocket(handle_),
        reinterpret_cast<char*>(buffer.Data()),
        static_cast<int>(buffer.Size()),
        0);

    // recv() returning 0 means the peer closed the connection gracefully
    // — deliberately not treated as an error, per Socket.md (mirrors
    // File::Read's own end-of-file convention exactly).
    if (received == SOCKET_ERROR)
    {
        return Result<SizeType>(Failure{ TranslateWinsockError(WSAGetLastError()) });
    }

    return Result<SizeType>(static_cast<SizeType>(received));
}

void Socket::Close() noexcept
{
    if (handle_ != nullptr)
    {
        closesocket(NativeSocket(handle_));
        handle_ = nullptr;
        isListening_ = false;
    }
}

} // namespace forge::core
