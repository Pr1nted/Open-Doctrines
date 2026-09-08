#pragma once

#include <atomic>
#include <string>
#include <vector>

// curl, run as a child process and NEVER through a shell.
//
// Two callers need this -- the game updater and the tunnel helper -- and they
// had two different answers. GameUpdates ran curl as a child with no shell and
// said so in a comment; TunnelInstall built a string with single-quoted
// arguments and handed it to std::system(). The second is wrong twice over:
// single quotes are not quoting in cmd.exe but ordinary characters, so it could
// never have worked on Windows at all, and interpolating a URL that came from a
// remote server into a shell command is the exact shape of a command injection.
//
// One implementation, so the two cannot drift again.
namespace odproc {

// Returns true only if curl exited 0.
//
// `args` are passed as separate argv entries, so a space or a quote in a path
// is data and can never be read as another argument or as a command.
//
// When `expectedSize` and `percent` are both given, `percent` is updated from
// the size of `progressFile` while the child runs -- output goes to a file
// rather than a pipe, so there is no stdout plumbing to get wrong on either
// platform and the partly-written file is what progress measures.
bool runCurl(const std::vector<std::string>& args,
             long long expectedSize = 0,
             std::atomic<int>* percent = nullptr,
             const std::string& progressFile = std::string());

// The same thing for any other program. runCurl is this with "curl".
//
// Used for tar, which unpacks the macOS cloudflared download. That was a shell
// string too, with single-quoted paths -- so a home directory containing an
// apostrophe, which is an ordinary thing for a surname to have, ended the quote
// early and the unpack failed. As argv there is nothing to quote and nothing to
// end.
bool runTool(const std::string& program,
             const std::vector<std::string>& args);

/**
 * Start a program and DO NOT wait for it. Returns its pid, or 0.
 *
 * For a server rather than a tool: `ollama serve` never exits, so runTool waits
 * for it forever. The child is put in its own session, so it is not killed by a
 * signal sent to the game's process group and does not take the game down with
 * it if it dies.
 *
 * The caller owns the pid and is responsible for stopping it -- see
 * stopDetached. A server left running after the game exits is a process the
 * player did not ask for and cannot see.
 */
long long startDetached(const std::string& program,
                        const std::vector<std::string>& args);

/// Stop a process started by startDetached. False if it was not running.
bool stopDetached(long long pid);

/// Whether that pid is still alive.
bool detachedAlive(long long pid);

}  // namespace odproc
