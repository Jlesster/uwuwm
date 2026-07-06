#pragma once

extern "C" {
#include <wayland-server-core.h>
}

#include <cstdint>
#include <list>
#include <memory>
#include <string>

class Server;
struct View;
struct Output;

// A minimal line-based UNIX-socket IPC server, listening at
// $XDG_RUNTIME_DIR/uwuwm-<WAYLAND_DISPLAY>.sock (falling back to
// /tmp/uwuwm-<WAYLAND_DISPLAY>.sock if XDG_RUNTIME_DIR isn't set).
// Exists so an external tool (a waybar/eww/AGS status-bar module, a
// `uwuwmctl` shell script, ...) can query compositor state and issue the
// same actions a keybind/rc.lua script can, without going through Lua or
// linking against uwuwm at all -- just a socket and newline-delimited
// text. Deliberately hand-rolled rather than pulling in a JSON library:
// meson.build has none, and the handful of response shapes below are
// fixed and simple enough that a tiny escaping helper (see ipc.cpp) is
// less code and less risk than a new dependency.
//
// Wire protocol: newline-delimited UTF-8, one request per line in, one
// response per line out (a JSON value for queries, or a bare
// "ok"/"error: <reason>" for commands):
//
//   get_outputs                    -> JSON array of output snapshots
//   get_clients                    -> JSON array of client snapshots
//   get_tags <output>               -> {"tagset": N} or an error
//   focus_tag <n> [output]         -> ok / error
//   set_tag <client_id> <n>        -> ok / error
//   close <client_id>              -> ok / error
//   close_all_on_tag <n>           -> ok / error
//   reload                         -> ok / error
//   subscribe <events>             -> ok, then this connection stops
//                                     being expected to send further
//                                     requests and instead receives one
//                                     JSON event object per line for as
//                                     long as it stays connected.
//                                     <events> is a comma-separated
//                                     subset of "client,output,tag", or
//                                     "*" for all three.
//
// A malformed or unrecognised request line gets "error: <reason>" back,
// never a dropped connection -- one bad line from a hand-written script
// shouldn't need a reconnect.
class IpcServer {
public:
    explicit IpcServer(Server& server);
    ~IpcServer();

    IpcServer(const IpcServer&)            = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    // Called from the exact same call sites LuaConfig::fireClientEvent/
    // fireOutputEvent/fireTagChange already fire from (toplevel.cpp,
    // xwayland_view.cpp, view.cpp, server.cpp, output.cpp) -- see those
    // functions' call sites. Subscribers see the same events, at the
    // same time, as uwu.hook("client::*"/"output::*"/"tag::change", fn)
    // Lua callbacks do; this is a second *listener* of those call sites,
    // not a second, possibly-divergent notion of when they fire.
    void onClientEvent(const std::string& event, View* view);
    void onOutputEvent(const std::string& event,
                       const std::string& output_name);
    void onTagChange(const std::string& output_name, uint32_t new_tagset);

    // Path the socket was actually bound to (empty if setup failed) --
    // logged at startup so `uwuwmctl`/a bar module knows where to look
    // without having to reimplement the $XDG_RUNTIME_DIR fallback logic.
    const std::string& socketPath() const { return socket_path_; }

private:
    struct Connection;

    static int handleAcceptTrampoline(int fd, uint32_t mask, void* data);
    static int handleConnectionTrampoline(int fd, uint32_t mask, void* data);

    void handleAccept();
    void handleConnectionReady(Connection& conn, uint32_t mask);
    void closeConnection(Connection& conn);

    // Parses and executes one request line, appending its response (plus
    // a trailing '\n') to conn.outbuf. Never throws/propagates a parse
    // failure past this -- see the class comment's "never a dropped
    // connection" guarantee.
    void handleRequestLine(Connection& conn, const std::string& line);

    // Queues `line` (a complete JSON object, no trailing newline) on
    // every connection currently subscribed to `category`
    // ("client"/"output"/"tag"), then tries to flush each immediately.
    void broadcast(const std::string& category, const std::string& json_line);

    // Registers WL_EVENT_WRITABLE on conn's fd source iff outbuf is
    // non-empty and it isn't already registered -- see wl_event_source_
    // fd_update's doc comment on why this only needs polling for
    // writable once a write has actually blocked.
    void tryFlush(Connection& conn);

    Server&          server_;
    std::string      socket_path_;
    int              listen_fd_     = -1;
    wl_event_source* listen_source_ = nullptr;

    std::list<std::unique_ptr<Connection>> connections_;
};
