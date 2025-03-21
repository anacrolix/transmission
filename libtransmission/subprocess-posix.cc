/*
 * This file Copyright (C) 2010-2017 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <cerrno>
#include <csignal>
#include <cstdlib>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "transmission.h"
#include "error.h"
#include "subprocess.h"
#include "tr-assert.h"
#include "tr-macros.h"
#include "utils.h"

static void handle_sigchld(int /*i*/)
{
    int rc = 0;

    do
    {
        /* FIXME: Only check for our own PIDs */
        rc = waitpid(-1, nullptr, WNOHANG);
    } while (rc > 0 || (rc == -1 && errno == EINTR));

    /* FIXME: Call old handler, if any */
}

static void set_system_error(tr_error** error, int code, char const* what)
{
    if (error == nullptr)
    {
        return;
    }

    if (what == nullptr)
    {
        tr_error_set_literal(error, code, tr_strerror(code));
    }
    else
    {
        tr_error_set(error, code, "%s failed: %s", what, tr_strerror(code));
    }
}

static bool tr_spawn_async_in_child(
    char const* const* cmd,
    std::map<std::string_view, std::string_view> const& env,
    char const* work_dir,
    int pipe_fd)
{
    auto key_sz = std::string{};
    auto val_sz = std::string{};

    for (auto const& [key_sv, val_sv] : env)
    {
        key_sz = key_sv;
        val_sz = val_sv;

        if (setenv(key_sz.c_str(), val_sz.c_str(), 1) != 0)
        {
            goto FAIL;
        }
    }

    if (work_dir != nullptr && chdir(work_dir) == -1)
    {
        goto FAIL;
    }

    if (execvp(cmd[0], const_cast<char* const*>(cmd)) == -1)
    {
        goto FAIL;
    }

    return true;

FAIL:
    (void)!write(pipe_fd, &errno, sizeof(errno));
    return false;
}

static bool tr_spawn_async_in_parent(int pipe_fd, tr_error** error)
{
    int child_errno = 0;
    ssize_t count = 0;

    static_assert(sizeof(child_errno) == sizeof(errno), "");

    do
    {
        count = read(pipe_fd, &child_errno, sizeof(child_errno));
    } while (count == -1 && errno == EINTR);

    close(pipe_fd);

    if (count == -1)
    {
        /* Read failed (what to do?) */
    }
    else if (count == 0)
    {
        /* Child successfully exec-ed */
    }
    else
    {
        TR_ASSERT((size_t)count == sizeof(child_errno));

        set_system_error(error, child_errno, "Child process setup");
        return false;
    }

    return true;
}

bool tr_spawn_async(
    char const* const* cmd,
    std::map<std::string_view, std::string_view> const& env,
    char const* work_dir,
    tr_error** error)
{
    static bool sigchld_handler_set = false;

    if (!sigchld_handler_set)
    {
        /* FIXME: "The effects of signal() in a multithreaded process are unspecified." (c) man 2 signal */
        if (signal(SIGCHLD, &handle_sigchld) == SIG_ERR) // NOLINT(performance-no-int-to-ptr)
        {
            set_system_error(error, errno, "Call to signal()");
            return false;
        }

        sigchld_handler_set = true;
    }

    int pipe_fds[2];

    if (pipe(pipe_fds) == -1)
    {
        set_system_error(error, errno, "Call to pipe()");
        return false;
    }

    if (fcntl(pipe_fds[1], F_SETFD, fcntl(pipe_fds[1], F_GETFD) | FD_CLOEXEC) == -1)
    {
        set_system_error(error, errno, "Call to fcntl()");
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return false;
    }

    int const child_pid = fork();

    if (child_pid == -1)
    {
        set_system_error(error, errno, "Call to fork()");
        return false;
    }

    if (child_pid == 0)
    {
        close(pipe_fds[0]);

        if (!tr_spawn_async_in_child(cmd, env, work_dir, pipe_fds[1]))
        {
            _exit(0);
        }
    }

    close(pipe_fds[1]);

    return tr_spawn_async_in_parent(pipe_fds[0], error);
}
