// SpeedBreaker launcher. GPL-3.0-or-later (see COPYING).
#include "process.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace launcher
{
    Process::~Process()
    {
        if (m_fd >= 0)
            close(m_fd);
    }

    bool Process::Start(const std::vector<std::string>& argv, const std::vector<std::string>& env, const std::string& cwd)
    {
        if (argv.empty())
            return false;
        int pipeFds[2];
        if (pipe2(pipeFds, O_CLOEXEC) != 0)
            return false;
        // Built before fork: the child only execs.
        std::vector<char*> args;
        for (const std::string& a : argv)
            args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        std::vector<std::string> envStrings;
        for (char** e = environ; *e; e++)
        {
            std::string s(*e);
            bool replaced = false;
            for (const std::string& x : env)
                if (s.compare(0, s.find('=') + 1, x, 0, x.find('=') + 1) == 0)
                    replaced = true;
            if (!replaced)
                envStrings.push_back(std::move(s));
        }
        for (const std::string& x : env)
            envStrings.push_back(x);
        std::vector<char*> envp;
        for (std::string& s : envStrings)
            envp.push_back(s.data());
        envp.push_back(nullptr);

        pid_t pid = fork();
        if (pid < 0)
        {
            close(pipeFds[0]);
            close(pipeFds[1]);
            return false;
        }
        if (pid == 0)
        {
            dup2(pipeFds[1], 1);
            dup2(pipeFds[1], 2);
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0)
                dup2(devnull, 0);
            setsid();
            if (!cwd.empty() && chdir(cwd.c_str()) != 0)
                _exit(126);
            execve(args[0], args.data(), envp.data());
            _exit(127);
        }
        close(pipeFds[1]);
        fcntl(pipeFds[0], F_SETFL, O_NONBLOCK);
        if (m_fd >= 0)
            close(m_fd);
        m_fd = pipeFds[0];
        m_pid = pid;
        m_exit = -1;
        m_partial.clear();
        return true;
    }

    std::vector<std::string> Process::Poll()
    {
        std::vector<std::string> lines;
        if (m_fd < 0)
            return lines;
        char buf[8192];
        for (;;)
        {
            ssize_t n = read(m_fd, buf, sizeof(buf));
            if (n == 0)
            {
                // End of output: a last line without its newline is a line too.
                if (!m_partial.empty())
                    lines.push_back(std::move(m_partial));
                m_partial.clear();
                break;
            }
            if (n < 0)
                break;
            for (ssize_t i = 0; i < n; i++)
            {
                if (buf[i] == '\n' || buf[i] == '\r')
                {
                    if (!m_partial.empty())
                        lines.push_back(std::move(m_partial));
                    m_partial.clear();
                }
                else
                    m_partial.push_back(buf[i]);
            }
        }
        return lines;
    }

    bool Process::Running()
    {
        if (m_pid < 0)
            return false;
        int status;
        pid_t r = waitpid(m_pid, &status, WNOHANG);
        if (r == 0)
            return true;
        if (r == m_pid)
            m_exit = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        m_pid = -1;
        return false;
    }

    void Process::Stop()
    {
        if (m_pid < 0)
            return;
        kill(-m_pid, SIGTERM);
        for (int i = 0; i < 50 && Running(); i++)
            usleep(100000);
        if (m_pid >= 0)
        {
            kill(-m_pid, SIGKILL);
            int status;
            waitpid(m_pid, &status, 0);
            m_pid = -1;
        }
    }
}
