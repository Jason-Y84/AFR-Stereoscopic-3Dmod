// winsock2 must precede windows.h.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "opentrack_receiver.h"
#include "logger.h"
#include <cmath>
#include <cstring>

namespace P5HT {

static long long NowMs() { return (long long)GetTickCount64(); }

unsigned long __stdcall OpenTrackReceiver::ThreadThunk(void* self) {
    static_cast<OpenTrackReceiver*>(self)->Run();
    return 0;
}

bool OpenTrackReceiver::Start(uint16_t port) {
    m_port = port;
    m_stop = false;
    m_thread = CreateThread(nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(&OpenTrackReceiver::ThreadThunk),
        this, 0, nullptr);
    return m_thread != nullptr;
}

void OpenTrackReceiver::Stop() {
    m_stop = true;
    if (m_thread) {
        WaitForSingleObject(static_cast<HANDLE>(m_thread), 1000);
        CloseHandle(static_cast<HANDLE>(m_thread));
        m_thread = nullptr;
    }
}

bool OpenTrackReceiver::IsReceiving() const {
    return m_running.load() && (NowMs() - m_lastRecvMs.load()) < 500;
}

Pose OpenTrackReceiver::Latest() const {
    Pose p;
    p.x = m_x.load(); p.y = m_y.load(); p.z = m_z.load();
    p.yaw = m_yaw.load(); p.pitch = m_pitch.load(); p.roll = m_roll.load();
    return p;
}

void OpenTrackReceiver::Run() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { Log("WSAStartup failed"); return; }

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { Log("socket() failed"); WSACleanup(); return; }

    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuse), sizeof(reuse));

    DWORD bytesRet = 0; BOOL connReset = FALSE;
    WSAIoctl(s, 0x9800000C /*SIO_UDP_CONNRESET*/, &connReset, sizeof(connReset),
             nullptr, 0, &bytesRet, nullptr, nullptr);

    sockaddr_in addr; std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // loopback AND remote trackers
    addr.sin_port = htons(m_port);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        Log("bind() failed on UDP port %u", (unsigned)m_port);
        closesocket(s); WSACleanup(); return;
    }
    DWORD timeout = 200;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));

    m_running = true;
    Log("OpenTrack receiver listening on 0.0.0.0:%u (all interfaces)", (unsigned)m_port);

    char buf[256];
    while (!m_stop.load()) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n >= 48) {
            double d[6];
            std::memcpy(d, buf, sizeof(d));  // x,y,z (cm), yaw,pitch,roll (deg)
            float fx = (float)(d[0] * 0.01), fy = (float)(d[1] * 0.01), fz = (float)(d[2] * 0.01);
            float fyaw = (float)d[3], fpitch = (float)d[4], froll = (float)d[5];
            if (std::isfinite(fx) && std::isfinite(fy) && std::isfinite(fz) &&
                std::isfinite(fyaw) && std::isfinite(fpitch) && std::isfinite(froll)) {
                m_x = fx; m_y = fy; m_z = fz;
                m_yaw = fyaw; m_pitch = fpitch; m_roll = froll;
                m_lastRecvMs = NowMs();
            }
        }
    }
    m_running = false;
    closesocket(s);
    WSACleanup();
}

} // namespace P5HT
