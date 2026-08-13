#pragma once
// OpenTrack UDP receiver. Win32 threads (no std::thread) so it builds on any
// toolchain. No <windows.h> here to avoid the winsock2 ordering clash; HANDLE is
// void*, the thread proc uses the raw __stdcall signature.
#include <atomic>
#include <cstdint>

namespace P5HT {

struct Pose {
    float x = 0, y = 0, z = 0;          // metres
    float yaw = 0, pitch = 0, roll = 0; // degrees
};

class OpenTrackReceiver {
public:
    ~OpenTrackReceiver() { Stop(); }
    bool Start(uint16_t port);
    void Stop();
    bool IsReceiving() const;
    Pose Latest() const;

private:
    static unsigned long __stdcall ThreadThunk(void* self);
    void Run();

    uint16_t m_port = 4242;
    void*     m_thread = nullptr;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_running{false};
    std::atomic<long long> m_lastRecvMs{0};
    std::atomic<float> m_x{0}, m_y{0}, m_z{0}, m_yaw{0}, m_pitch{0}, m_roll{0};
};

} // namespace P5HT
