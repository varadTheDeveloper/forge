#pragma once

#include "Error.h"
#include "IpAddress.h"
#include "Result.h"
#include "Span.h"
#include "Types.h"

namespace forge::core
{

/// Synchronous, blocking TCP socket — connect out as a client, or
/// listen/accept as a server, then send/receive bytes. See Socket.md for
/// the full spec, in particular: Windows/Winsock-only (no other backend
/// exists), and deliberately synchronous (no IocpLoop dependency) — an
/// async variant is future work for whenever Runtime Integration needs
/// one. This is the only forge-core type allowed to call into Winsock;
/// `IpAddress`/`Endpoint` (see IpAddress.md) describe *where*, this is
/// *how* — same `Path`/`File` split Phase 3 established, applied here.
///
/// Move-only, like `File` — a Winsock `SOCKET` has no well-defined copy
/// operation.
///
/// Cannot be compiled or run in the sandbox this was written in (no
/// Windows SDK, no working MinGW cross-compiler available) — verified
/// by careful manual review, plus a hand-written mock <winsock2.h>
/// compile+link pass. Needs a real mach build (or a standalone Visual
/// Studio smoke test, see SocketSmokeTest.cpp) to go from "implemented"
/// to "confirmed working".
class [[nodiscard]] Socket
{
public:

    using SizeType = forge::core::Size;

    Socket() noexcept;

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    ~Socket() noexcept;

    /// Connects to `remote` as a client. Blocks until the connection
    /// succeeds or fails.
    [[nodiscard]]
    static Result<Socket> Connect(
        const Endpoint& remote);

    /// Binds to `local` and starts listening for incoming connections.
    /// `backlog` is the OS listen-queue depth (internally capped at
    /// SOMAXCONN). The returned Socket is only valid to call Accept() on
    /// — Send()/Receive() are for a connected socket (either one
    /// returned by Connect(), or one returned by this listening socket's
    /// own Accept()).
    [[nodiscard]]
    static Result<Socket> Listen(
        const Endpoint& local,
        int backlog = 128);

    /// Blocks until an incoming connection arrives on a socket created
    /// via Listen(), then returns it as a new, connected Socket. Calling
    /// this on a socket not created via Listen() is a programming error
    /// (ErrorCode::InvalidOperation).
    [[nodiscard]]
    Result<Socket> Accept();

    /// Same as Accept(), and also reports the connecting peer's address
    /// in `peer` (Phase 7.9: the plain Accept() discards it). An
    /// IPv4-mapped IPv6 peer ("::ffff:a.b.c.d", reported by dual-stack
    /// listeners) is returned as the plain IPv4 address, so callers
    /// comparing peers against allow-lists see one form only.
    [[nodiscard]]
    Result<Socket> Accept(
        Endpoint& peer);

    [[nodiscard]]
    bool IsOpen() const noexcept;

    /// Writes up to buffer.Size() bytes. Returns the number actually
    /// sent — a short send is surfaced as-is, not silently retried into
    /// a full send, mirroring File::Write's own policy exactly.
    [[nodiscard]]
    Result<SizeType> Send(
        Span<const u8> buffer);

    /// Reads up to buffer.Size() bytes. Returns the number actually
    /// received — 0 means the peer closed the connection gracefully,
    /// which is not itself an error, mirroring File::Read's own
    /// end-of-file convention exactly.
    [[nodiscard]]
    Result<SizeType> Receive(
        Span<u8> buffer);

    void Close() noexcept;

private:

    explicit Socket(
        void* handle) noexcept;

private:

    // Kept as void* (an opaque Winsock SOCKET) rather than including
    // <winsock2.h> here — SOCKET is UINT_PTR (pointer-sized on every
    // Windows target this project builds for), so the reinterpret_cast
    // to/from void* is a legal integral<->pointer conversion. Same
    // reasoning File.h applies to its own HANDLE-as-void*, and IocpLoop
    // applies to its OVERLAPPED-derived types: keep the platform header
    // confined to the .cpp, out of every translation unit that just
    // wants to open a connection.
    void* handle_{ nullptr };

    // Distinguishes a listening socket (Accept() is valid, Send()/
    // Receive() are not) from a connected one (the reverse) — both
    // share the same handle_ representation, so this flag is the only
    // thing telling them apart at this layer.
    bool isListening_{ false };
};

} // namespace forge::core
