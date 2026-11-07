
#pragma once

#include <atomic>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace lva::tr {

class Supervisor;

class SupervisorHttpServer {
   public:
    SupervisorHttpServer(Supervisor& supervisor, std::uint16_t port = 8086);
    ~SupervisorHttpServer();

    SupervisorHttpServer(const SupervisorHttpServer&)            = delete;
    SupervisorHttpServer& operator=(const SupervisorHttpServer&) = delete;

    bool Start();

    void Stop();

   private:
    void AcceptLoop();
    bool WaitForActionDelay(const char* action);
    void HandleConnection(int conn_fd);
    void HandleGet(int conn_fd, const std::string& path);
    void HandlePost(int conn_fd, const std::string& path,
                    const std::string& content_type,
                    const std::string& body);
    void SendJson(int conn_fd, int status, const std::string& json);
    void SendText(int conn_fd, int status, const std::string& body);

    Supervisor&         supervisor_;
    std::uint16_t       port_;
    int                 listen_fd_      = -1;
    std::atomic<bool>   running_{false};
    std::thread         accept_thread_;

    // Slots are reused only after joining the previous connection thread.
    static constexpr std::size_t kMaxConnections = 8;
    struct Worker {
        std::thread thread;
        int fd = -1;  // Protected by workers_mutex_, including close().
    };
    std::array<Worker, kMaxConnections> workers_;
    std::mutex workers_mutex_;
    std::condition_variable stopped_;
};

}  // namespace lva::tr
