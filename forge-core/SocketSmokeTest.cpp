// Real-network smoke test for Socket — same role as
// ThreadingSmokeTest.cpp/FileSmokeTest.cpp: this binds a real loopback
// TCP listener and connects a real client to it, so it can ONLY be built
// and run on an actual Windows machine (e.g. via a standalone Visual
// Studio project, same as ThreadingSmokeTest.cpp's precedent), never in
// the Linux sandbox this was written in (no Windows SDK / working
// MinGW cross compiler available there — Socket.cpp itself was only
// compile+link verified against a hand-written mock <winsock2.h>, see
// Socket.md's Acceptance Criteria). Not part of the production
// moz.build build (matches every other *SmokeTest.cpp in this
// codebase).
//
// Build (run from this file's own directory, forge-core\, so the
// unprefixed paths below resolve correctly):
//   cl /std:c++17 /EHs- /W4 SocketSmokeTest.cpp
//   Socket.cpp Mutex.cpp ConditionVariable.cpp Thread.cpp
//   platform\Win32Error.cpp memory\DefaultAllocator.cpp
//   memory\detail\AllocationBackend.cpp /link ws2_32.lib
//
// Note: IpAddress.inl is deliberately NOT listed as a source file here.
// It's included by IpAddress.h (like every other *.inl in this codebase
// — Path.inl, Queue.inl, etc.), not meant to be compiled as its own
// translation unit; its member-function definitions reference IpAddress/
// Endpoint, which are only declared once IpAddress.h has already been
// processed. Passing it to cl directly would fail to compile with
// undeclared-identifier errors, not silently double-include harmlessly.

#include "IpAddress.h"
#include "Socket.h"

#include "ConditionVariable.h"
#include "LockGuard.h"
#include "Mutex.h"
#include "Thread.h"

#include <cstdio>
#include <cstring>
#include <utility>

using namespace forge::core;

namespace
{

int g_failures = 0;

// File-scope, not a local inside RunSocketEchoTest, deliberately: the
// server-thread lambda below uses this both as an array bound (which
// must be a real compile-time constant expression) and as a Span byte
// count. A local constexpr would need to be captured to be visible
// inside the lambda, but capturing it makes every use inside the lambda
// refer to the closure's own (non-constexpr) copy of the value instead
// of the original constant — which breaks the array-bound use with a
// real MSVC error (C2131, "expression did not evaluate to a constant";
// see HISTORY.md). GCC/Clang don't even require the capture in the first
// place for a read-only constexpr, which is why this was never caught
// outside a real MSVC compile. File scope sidesteps the whole issue:
// nothing here is a captured local, so it's a genuine constant
// expression everywhere it's used, capture or no capture.
constexpr Size kPayloadSize = 5;

#define CHECK(cond)                                                          \
    do                                                                       \
    {                                                                        \
        if (!(cond))                                                        \
        {                                                                    \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__,    \
                          __LINE__, #cond);                                  \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

/// Reads exactly `buffer.Size()` bytes from `socket`, looping over
/// Receive() the same way File::ReadAllBytes() loops over File::Read()
/// — a single Receive() call is not guaranteed to fill the buffer even
/// when the peer sends it all in one Send(), since TCP has no message
/// boundaries.
Result<void> ReceiveExact(
    Socket& socket,
    Span<u8> buffer)
{
    Size totalReceived = 0;

    while (totalReceived < buffer.Size())
    {
        Span<u8> remaining(buffer.Data() + totalReceived, buffer.Size() - totalReceived);
        Result<Size> received = socket.Receive(remaining);

        if (received.HasError())
        {
            return Result<void>(Failure{ received.Error() });
        }

        if (received.Value() == 0)
        {
            // Peer closed early — short of a full buffer is a test
            // failure here (a real caller would treat this as a
            // legitimate, non-error "connection ended" condition, per
            // Socket.md, but this test expects a specific byte count).
            return Result<void>(Failure{ Error(ErrorCode::IOError) });
        }

        totalReceived += received.Value();
    }

    return {};
}

/// Listens on a fixed loopback port, accepts exactly one connection,
/// echoes back whatever it receives (up to kPayloadSize bytes), then
/// closes — run on a background Thread while the main thread plays
/// client, exercising Connect/Listen/Accept/Send/Receive/Close all
/// against a real OS TCP stack end to end.
void RunSocketEchoTest()
{
    std::printf("RunSocketEchoTest...\n");

    constexpr u16 kPort = 53421; // arbitrary fixed high port, unlikely
                                  // to collide with anything else on a
                                  // CI/dev machine; see Socket.md's
                                  // Implementation Status re: this
                                  // first cut's Endpoint::Parse having
                                  // no ephemeral-port ("127.0.0.1:0")
                                  // support yet.
    const u8 payload[kPayloadSize] = { 'f', 'o', 'r', 'g', 'e' };

    const Endpoint endpoint(IpAddress::V4(127, 0, 0, 1), kPort);

    Result<Socket> listener = Socket::Listen(endpoint, /*backlog=*/1);
    CHECK(listener.HasValue());

    if (!listener.HasValue())
    {
        return; // nothing further to test without a real listener
    }

    // The listening socket is moved into the server thread's closure —
    // Socket is move-only, matching File, so this is the same "hand
    // ownership to the thread that will use it" pattern
    // ThreadingSmokeTest.cpp's producer/consumer test uses for shared
    // state, just via move instead of by-reference capture.
    bool serverOk = true;
    bool peerIsLoopback = false;

    Result<Thread> server = Thread::Create(
        [listenerSocket = std::move(listener.Value()), &serverOk, &peerIsLoopback]() mutable {
            // Phase 7.9: Accept(peer) reports the client's address.
            Endpoint peer;
            Result<Socket> accepted = listenerSocket.Accept(peer);

            if (!accepted.HasValue())
            {
                serverOk = false;
                return;
            }

            peerIsLoopback = peer.Address() == IpAddress::V4(127, 0, 0, 1) && peer.Port() != 0;

            Socket connection = std::move(accepted.Value());

            u8 received[kPayloadSize]{};
            Result<void> receivedAll = ReceiveExact(connection, Span<u8>(received, kPayloadSize));

            if (receivedAll.HasError())
            {
                serverOk = false;
                return;
            }

            Result<Size> sent = connection.Send(Span<const u8>(received, kPayloadSize));

            if (sent.HasError() || sent.Value() != kPayloadSize)
            {
                serverOk = false;
                return;
            }

            connection.Close();
            listenerSocket.Close();
        });

    CHECK(server.HasValue());

    if (!server.HasValue())
    {
        return;
    }

    // A real client connecting to a listening socket that has already
    // called listen() succeeds as soon as the OS accepts it into the
    // backlog queue — Accept() doesn't have to have been called yet —
    // so no extra readiness signal (Mutex/ConditionVariable) is needed
    // between spawning the server thread and connecting here.
    Result<Socket> client = Socket::Connect(endpoint);
    CHECK(client.HasValue());

    if (client.HasValue())
    {
        Result<Size> sent = client.Value().Send(Span<const u8>(payload, kPayloadSize));
        CHECK(sent.HasValue());
        CHECK(sent.HasValue() && sent.Value() == kPayloadSize);

        u8 echoed[kPayloadSize]{};
        Result<void> receivedAll = ReceiveExact(client.Value(), Span<u8>(echoed, kPayloadSize));
        CHECK(!receivedAll.HasError());

        CHECK(std::memcmp(payload, echoed, kPayloadSize) == 0);

        client.Value().Close();
        CHECK(!client.Value().IsOpen());
    }

    CHECK(!server.Value().Join().HasError());
    CHECK(serverOk);
    CHECK(peerIsLoopback);
}

/// Phase 7.9: Listen() used to set SO_REUSEADDR, which on Windows lets a
/// second socket bind the same port and steal connections. With
/// SO_EXCLUSIVEADDRUSE a second Listen() on a port already in use must
/// fail.
void RunSocketExclusiveBindTest()
{
    std::printf("RunSocketExclusiveBindTest...\n");

    const Endpoint endpoint(IpAddress::V4(127, 0, 0, 1), 53423);

    Result<Socket> first = Socket::Listen(endpoint, /*backlog=*/1);
    CHECK(first.HasValue());

    if (!first.HasValue())
    {
        return;
    }

    Result<Socket> second = Socket::Listen(endpoint, /*backlog=*/1);
    CHECK(second.HasError());
}

/// Connect() to a port nothing is listening on must fail with a
/// Result error, never crash or hang indefinitely.
void RunSocketConnectRefusedTest()
{
    std::printf("RunSocketConnectRefusedTest...\n");

    // Port chosen one above the echo test's — still expected to have
    // nothing listening on a normal dev/CI machine.
    const Endpoint endpoint(IpAddress::V4(127, 0, 0, 1), 53422);

    Result<Socket> client = Socket::Connect(endpoint);
    CHECK(client.HasError());
}

} // namespace

int main()
{
    RunSocketEchoTest();
    RunSocketConnectRefusedTest();
    RunSocketExclusiveBindTest();

    if (g_failures == 0)
    {
        std::printf("SocketSmokeTest: all checks passed\n");
        return 0;
    }

    std::printf("SocketSmokeTest: %d CHECK(S) FAILED\n", g_failures);
    return 1;
}
