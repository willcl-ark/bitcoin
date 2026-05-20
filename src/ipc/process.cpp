// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <ipc/process.h>
#include <ipc/protocol.h>
#include <logging.h>
#include <tinyformat.h>
#include <util/fs.h>
#include <util/strencodings.h>
#include <util/syserror.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <limits>
#include <cstdio>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <vector>

using util::RemovePrefixView;

namespace ipc {
namespace {
std::vector<char*> MakeArgv(const std::vector<std::string>& args)
{
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

int MaxFd()
{
    struct rlimit nofile;
    if (::getrlimit(RLIMIT_NOFILE, &nofile) == 0 && nofile.rlim_cur > 0) {
        return static_cast<int>(std::min<rlim_t>(nofile.rlim_cur - 1, std::numeric_limits<int>::max()));
    }
    return 1023;
}

class ProcessImpl : public Process
{
public:
    int spawn(const std::string& new_exe_name, const fs::path& argv0_path, int& pid) override
    {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            throw std::system_error(errno, std::system_category(), "socketpair");
        }

        fs::path path = argv0_path;
        path.remove_filename();
        path /= fs::PathFromString(new_exe_name);
        const std::vector<std::string> args{fs::PathToString(path), "-ipcfd", strprintf("%i", fds[0])};
        const std::vector<char*> argv{MakeArgv(args)};

        pid = ::fork();
        if (pid == -1) {
            const int fork_error{errno};
            (void)::close(fds[0]);
            (void)::close(fds[1]);
            throw std::system_error(fork_error, std::system_category(), "fork");
        }

        if (::close(fds[pid ? 0 : 1]) != 0) {
            if (pid) {
                const int close_error{errno};
                (void)::close(fds[1]);
                throw std::system_error(close_error, std::system_category(), "close");
            }
            static constexpr char msg[] = "ProcessImpl::spawn child close failed\n";
            const ssize_t write_result{::write(STDERR_FILENO, msg, sizeof(msg) - 1)};
            (void)write_result;
            _exit(126);
        }

        if (!pid) {
            const int max_fd{MaxFd()};
            for (int fd = 3; fd < max_fd; ++fd) {
                if (fd != fds[0]) (void)::close(fd);
            }

            ::execvp(argv[0], argv.data());
            perror("execvp failed");
            _exit(127);
        }

        return fds[1];
    }
    int waitSpawned(int pid) override
    {
        int status;
        if (::waitpid(pid, &status, /*options=*/0) != pid) {
            throw std::system_error(errno, std::system_category(), "waitpid");
        }
        return status;
    }
    bool checkSpawned(int argc, char* argv[], int& fd) override
    {
        // If this process was not started with a single -ipcfd argument, it is
        // not a process spawned by the spawn() call above, so return false and
        // do not try to serve requests.
        if (argc != 3 || strcmp(argv[1], "-ipcfd") != 0) {
            return false;
        }
        // If a single -ipcfd argument was provided, return true and get the
        // file descriptor so Protocol::serve() can be called to handle
        // requests from the parent process. The -ipcfd argument is not valid
        // in combination with other arguments because the parent process
        // should be able to control the child process through the IPC protocol
        // without passing information out of band.
        const auto maybe_fd{ToIntegral<int32_t>(argv[2])};
        if (!maybe_fd) {
            throw std::runtime_error(strprintf("Invalid -ipcfd number '%s'", argv[2]));
        }
        fd = *maybe_fd;
        return true;
    }
    int connect(const fs::path& data_dir,
                const std::string& dest_exe_name,
                std::string& address) override;
    int bind(const fs::path& data_dir, const std::string& exe_name, std::string& address) override;
};

static bool ParseAddress(std::string& address,
                  const fs::path& data_dir,
                  const std::string& dest_exe_name,
                  struct sockaddr_un& addr,
                  std::string& error)
{
    if (address == "unix" || address.starts_with("unix:")) {
        fs::path path;
        if (address.size() <= 5) {
            path = data_dir / fs::PathFromString(strprintf("%s.sock", RemovePrefixView(dest_exe_name, "bitcoin-")));
        } else {
            path = data_dir / fs::PathFromString(address.substr(5));
        }
        std::string path_str = fs::PathToString(path);
        address = strprintf("unix:%s", path_str);
        if (path_str.size() >= sizeof(addr.sun_path)) {
            error = strprintf("Unix address path %s exceeded maximum socket path length", fs::quoted(fs::PathToString(path)));
            return false;
        }
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path_str.c_str(), sizeof(addr.sun_path)-1);
        return true;
    }

    error = strprintf("Unrecognized address '%s'", address);
    return false;
}

int ProcessImpl::connect(const fs::path& data_dir,
                         const std::string& dest_exe_name,
                         std::string& address)
{
    struct sockaddr_un addr;
    std::string error;
    if (!ParseAddress(address, data_dir, dest_exe_name, addr, error)) {
        throw std::invalid_argument(error);
    }

    int fd;
    if ((fd = ::socket(addr.sun_family, SOCK_STREAM, 0)) == -1) {
        throw std::system_error(errno, std::system_category());
    }
    if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        return fd;
    }
    int connect_error = errno;
    if (::close(fd) != 0) {
        LogWarning("Error closing file descriptor %i '%s': %s", fd, address, SysErrorString(errno));
    }
    throw std::system_error(connect_error, std::system_category());
}

int ProcessImpl::bind(const fs::path& data_dir, const std::string& exe_name, std::string& address)
{
    struct sockaddr_un addr;
    std::string error;
    if (!ParseAddress(address, data_dir, exe_name, addr, error)) {
        throw std::invalid_argument(error);
    }

    if (addr.sun_family == AF_UNIX) {
        fs::path path = addr.sun_path;
        if (path.has_parent_path()) fs::create_directories(path.parent_path());
        if (fs::symlink_status(path).type() == fs::file_type::socket) {
            fs::remove(path);
        }
    }

    int fd;
    if ((fd = ::socket(addr.sun_family, SOCK_STREAM, 0)) == -1) {
        throw std::system_error(errno, std::system_category());
    }

    if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        return fd;
    }
    int bind_error = errno;
    if (::close(fd) != 0) {
        LogWarning("Error closing file descriptor %i: %s", fd, SysErrorString(errno));
    }
    throw std::system_error(bind_error, std::system_category());
}
} // namespace

std::unique_ptr<Process> MakeProcess() { return std::make_unique<ProcessImpl>(); }
} // namespace ipc
