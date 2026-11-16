// SpeedBreaker launcher. GPL-3.0-or-later (see COPYING).
// A child process whose output the UI reads as it comes (stdout and stderr
// merged), without blocking.
#pragma once
#include <string>
#include <utility>
#include <vector>
#include <sys/types.h>

namespace launcher
{
    class Process
    {
    public:
        ~Process();
        // argv[0] is the program (a path). env: extra NAME=value entries.
        bool Start(const std::vector<std::string>& argv, const std::vector<std::string>& env = {}, const std::string& cwd = {});
        // New complete lines since the last call (also splits at '\r').
        std::vector<std::string> Poll();
        bool Running();
        int ExitCode() const { return m_exit; }
        void Stop();  // SIGTERM, then waits
        pid_t Pid() const { return m_pid; }

    private:
        pid_t m_pid = -1;
        int m_fd = -1;
        int m_exit = -1;
        std::string m_partial;
    };
}
