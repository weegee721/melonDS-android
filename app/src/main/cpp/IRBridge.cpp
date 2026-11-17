// Pokewalker IR bridge for CartRetailIR (HeartGold/SoulSilver, Black/White).
//
// Wire-compatible with melonDS-IR's TCP mode: a TCP server on port 8081
// carrying raw (0xAA-XORed) IR bytes, one packet per write. The server binds to
// loopback only, so the peer is an app on this phone (the Artemis companion,
// which relays packets to the watch over Bluetooth).
//
// Latency: after the game transmits it expects the walker's reply within a
// few emulated milliseconds. While a reply is outstanding, the receive call
// blocks the emulator thread (freezing emulated time) for up to 150 ms of
// real time (melonDS-IR waits 150 ms; a Bluetooth relay needs longer). Idle
// polling never blocks.

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "Platform.h"
#include "MelonLog.h"

namespace melonDS::Platform
{

namespace
{
constexpr int Port = 8081;
// Emulated time is frozen while waiting, so the game cannot observe a longer
// wait; Bluetooth relays occasionally need several hundred ms.
constexpr u64 ReplyWaitMs = 2000;
int listener = -1;
int client = -1;
bool awaitingReply = false;

void setNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void closeClient()
{
    if (client >= 0) close(client);
    client = -1;
    awaitingReply = false;
}

void openServer()
{
    if (listener < 0)
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return;
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(Port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(fd, 1) != 0)
        {
            LOG_ERROR("IRBridge", "IR server could not listen on 127.0.0.1:%d (errno %d)", Port, errno);
            close(fd);
            return;
        }
        setNonBlocking(fd);
        listener = fd;
        LOG_INFO("IRBridge", "IR server listening on 127.0.0.1:%d", Port);
    }

    // Take the newest connection; a reconnecting peer replaces a stale one.
    while (true)
    {
        int fd = accept(listener, nullptr, nullptr);
        if (fd < 0) break;
        closeClient();
        setNonBlocking(fd);
        int yes = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        client = fd;
        LOG_INFO("IRBridge", "IR peer connected");
    }
}

int readAvailable(char* data, int len)
{
    ssize_t got = recv(client, data, len, 0);
    if (got > 0) return static_cast<int>(got);
    if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
    {
        LOG_INFO("IRBridge", "IR peer disconnected");
        closeClient();
    }
    return 0;
}
}

u8 IR_SendPacket(char* data, int len, void* userdata)
{
    (void)userdata;
    openServer();
    if (client < 0 || len <= 0) return 0;
    ssize_t sent = send(client, data, len, MSG_NOSIGNAL);
    if (sent != len)
    {
        closeClient();
        return 0;
    }
    awaitingReply = true;
    return 0;
}

u8 IR_RecievePacket(char* data, int len, void* userdata)
{
    (void)userdata;
    openServer();
    if (client < 0 || len <= 0) return 0;
    int got = readAvailable(data, len);
    if (got == 0 && awaitingReply && client >= 0)
    {
        u64 start = GetMSCount();
        while (client >= 0 && GetMSCount() - start < ReplyWaitMs)
        {
            usleep(1000);
            got = readAvailable(data, len);
            if (got > 0) break;
        }
        // No reply within the bound: let the game see silence and run its own
        // retry/timeout logic instead of stalling again on the next poll.
        if (got == 0) { awaitingReply = false; LOG_WARN("IRBridge", "no reply within %llu ms", (unsigned long long)ReplyWaitMs); }
        else LOG_DEBUG("IRBridge", "reply after %llu ms", (unsigned long long)(GetMSCount() - start));
    }
    if (got > 0) awaitingReply = false;
    return static_cast<u8>(got);
}

void IR_LogPacket(char* data, int len, bool isTx, void* userdata)
{
    (void)userdata;
    char head[3 * 8 + 1] = {};
    for (int i = 0; i < len && i < 8; ++i) snprintf(head + 3 * i, 4, "%02X ", (unsigned char)(data[i] ^ 0xAA));
    LOG_DEBUG("IRBridge", "%s %d bytes: %s", isTx ? "Tx" : "Rx", len, head);
}

}
