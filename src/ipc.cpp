#include "ipc.hpp"

#include "config.hpp"
#include "output.hpp"
#include "server.hpp"
#include "view.hpp"
#include "xwayland_view.hpp"

extern "C" {
#include <wlr/util/log.h>
}

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

namespace {

// Minimal JSON string escaping -- the fixed set of control characters
// JSON actually requires escaping, nothing fancier (no unicode
// normalization, no key sorting): title/app_id/output names are the
// only untrusted-ish strings that ever go through this, and none of them
// are expected to contain much beyond printable ASCII in practice, but a
// window with a newline or a literal quote in its title is a real thing
// a hostile or just weird client can produce, so this has to be right
// rather than "usually fine."
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for(unsigned char c : s) {
        switch(c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if(c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string outputSnapshot(Server& server, Output& out) {
    std::ostringstream os;
    os << "{\"name\":\"" << jsonEscape(out.wlr_output->name) << "\""
       << ",\"x\":" << out.layout_box.x << ",\"y\":" << out.layout_box.y
       << ",\"width\":" << out.layout_box.width
       << ",\"height\":" << out.layout_box.height
       << ",\"tagset\":" << out.tagset
       << ",\"focused\":" << (server.focused_output == &out ? "true" : "false")
       << "}";
    return os.str();
}

std::string clientSnapshot(Server& server, View& v) {
    std::ostringstream os;
    os << "{\"id\":" << v.id << ",\"title\":\"" << jsonEscape(v.title) << "\""
       << ",\"app_id\":\"" << jsonEscape(v.app_id) << "\""
       << ",\"is_xwayland\":"
       << (dynamic_cast<XWaylandView*>(&v) ? "true" : "false")
       << ",\"tags\":" << v.tags
       << ",\"floating\":" << (v.is_floating ? "true" : "false")
       << ",\"fullscreen\":" << (v.is_fullscreen ? "true" : "false")
       << ",\"output\":"
       << (v.output ? "\"" + jsonEscape(v.output->wlr_output->name) + "\""
                    : "null")
       << ",\"focused\":" << (server.focused_view == &v ? "true" : "false")
       << ",\"x\":" << v.geo.x << ",\"y\":" << v.geo.y
       << ",\"width\":" << v.geo.width << ",\"height\":" << v.geo.height << "}";
    return os.str();
}

// Splits a request line on whitespace. No quoting support -- every
// current request's arguments (client ids, tag numbers, output names)
// are single tokens, so this deliberately doesn't take on shell-style
// quoting complexity a client script would rarely need anyway (an
// output name with a space in it is vanishingly unlikely, and can still
// be passed as the request's *last* token by leaving it unsplit --see
// handleRequestLine's per-command arg count handling).
std::vector<std::string> splitTokens(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream       is(line);
    std::string              tok;
    while(is >> tok) { tokens.push_back(tok); }
    return tokens;
}

bool parseUInt(const std::string& s, uint32_t& out) {
    if(s.empty()) { return false; }
    char* end       = nullptr;
    errno           = 0;
    unsigned long v = std::strtoul(s.c_str(), &end, 10);
    if(errno != 0 || end != s.c_str() + s.size()) { return false; }
    out = static_cast<uint32_t>(v);
    return true;
}

}  // namespace

// Owns one accepted client connection: its fd, the wl_event_source
// watching it, and the in/out byte buffers a non-blocking socket needs.
struct IpcServer::Connection {
    IpcServer*       ipc    = nullptr;
    int              fd     = -1;
    wl_event_source* source = nullptr;
    std::string      inbuf;   // bytes read but not yet a full '\n'-line
    std::string      outbuf;  // bytes queued to write but not yet sent
    bool             watching_writable = false;

    // Which broadcast() categories this connection wants events for,
    // set by a "subscribe <events>" request line -- see class comment
    // in ipc.hpp. All false until subscribe is sent at least once.
    bool sub_client = false, sub_output = false, sub_tag = false;
};

IpcServer::IpcServer(Server& server) : server_(server) {
    const char* wayland_display = getenv("WAYLAND_DISPLAY");
    const char* runtime_dir     = getenv("XDG_RUNTIME_DIR");
    std::string dir = (runtime_dir && *runtime_dir) ? runtime_dir : "/tmp";
    std::string name =
        "uwuwm-" + std::string(wayland_display ? wayland_display : "unknown") +
        ".sock";
    std::string path = dir + "/" + name;

    sockaddr_un addr{};
    if(path.size() >= sizeof(addr.sun_path)) {
        wlr_log(WLR_ERROR, "IPC socket path too long: %s", path.c_str());
        return;
    }

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if(listen_fd_ < 0) {
        wlr_log(WLR_ERROR, "IPC socket() failed: %s", strerror(errno));
        return;
    }

    // A leftover socket file from a previous run that crashed/was
    // killed rather than shutting down cleanly (which would have
    // unlinked it -- see ~IpcServer below) would otherwise make bind()
    // fail forever on every subsequent launch. There's no lock-file
    // dance here the way wl_display_add_socket has for *its* sockets
    // (concurrent uwuwm instances aren't a thing this needs to guard
    // against the way multiple Wayland compositors racing for
    // wayland-0 is), so unlinking unconditionally before bind is safe.
    unlink(path.c_str());

    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    if(bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
       0) {
        wlr_log(WLR_ERROR,
                "IPC bind(%s) failed: %s",
                path.c_str(),
                strerror(errno));
        close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    if(listen(listen_fd_, 16) != 0) {
        wlr_log(WLR_ERROR, "IPC listen() failed: %s", strerror(errno));
        close(listen_fd_);
        listen_fd_ = -1;
        unlink(path.c_str());
        return;
    }

    wl_event_loop* loop = wl_display_get_event_loop(server_.display);
    listen_source_ = wl_event_loop_add_fd(loop,
                                          listen_fd_,
                                          WL_EVENT_READABLE,
                                          &IpcServer::handleAcceptTrampoline,
                                          this);
    if(!listen_source_) {
        wlr_log(WLR_ERROR, "IPC wl_event_loop_add_fd failed");
        close(listen_fd_);
        listen_fd_ = -1;
        unlink(path.c_str());
        return;
    }

    socket_path_ = path;
}

IpcServer::~IpcServer() {
    for(auto& conn : connections_) { closeConnection(*conn); }
    connections_.clear();

    if(listen_source_) { wl_event_source_remove(listen_source_); }
    if(listen_fd_ >= 0) { close(listen_fd_); }
    if(!socket_path_.empty()) { unlink(socket_path_.c_str()); }
}

int IpcServer::handleAcceptTrampoline(int /*fd*/,
                                      uint32_t /*mask*/,
                                      void* data) {
    static_cast<IpcServer*>(data)->handleAccept();
    return 0;
}

int IpcServer::handleConnectionTrampoline(int fd, uint32_t mask, void* data) {
    // `data` is the Connection*, stashed as the fd source's user data at
    // wl_event_loop_add_fd time in handleAccept() below.
    auto* conn = static_cast<Connection*>(data);
    (void)fd;
    conn->ipc->handleConnectionReady(*conn, mask);
    return 0;
}

void IpcServer::handleAccept() {
    for(;;) {
        int fd =
            accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if(fd < 0) {
            // EAGAIN/EWOULDBLOCK just means "no more pending connections
            // right now" -- expected, not an error worth logging. Any
            // other errno is unusual enough (EMFILE, ECONNABORTED, ...)
            // to be worth a log line, but still not fatal to the whole
            // IPC server -- one failed accept shouldn't take the socket
            // down for every future client.
            if(errno != EAGAIN && errno != EWOULDBLOCK) {
                wlr_log(WLR_ERROR, "IPC accept4() failed: %s", strerror(errno));
            }
            return;
        }

        auto conn = std::make_unique<Connection>();
        conn->ipc = this;
        conn->fd  = fd;

        wl_event_loop* loop = wl_display_get_event_loop(server_.display);
        conn->source =
            wl_event_loop_add_fd(loop,
                                 fd,
                                 WL_EVENT_READABLE,
                                 &IpcServer::handleConnectionTrampoline,
                                 conn.get());
        if(!conn->source) {
            wlr_log(WLR_ERROR, "IPC wl_event_loop_add_fd (client) failed");
            close(fd);
            continue;
        }

        connections_.push_back(std::move(conn));
    }
}

void IpcServer::closeConnection(Connection& conn) {
    if(conn.source) { wl_event_source_remove(conn.source); }
    if(conn.fd >= 0) { close(conn.fd); }
    conn.source = nullptr;
    conn.fd     = -1;
}

void IpcServer::handleConnectionReady(Connection& conn, uint32_t mask) {
    if(mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        connections_.remove_if([&](std::unique_ptr<Connection>& c) {
            if(c.get() != &conn) { return false; }
            closeConnection(*c);
            return true;
        });
        return;
    }

    if(mask & WL_EVENT_READABLE) {
        char buf[4096];
        for(;;) {
            ssize_t n = read(conn.fd, buf, sizeof buf);
            if(n > 0) {
                conn.inbuf.append(buf, static_cast<size_t>(n));
                continue;
            }
            if(n == 0) {
                // Peer closed its write side. Still flush any response
                // we owe it before dropping the connection -- a client
                // that half-closes right after its last request (a
                // one-shot `uwuwmctl get_clients | jq ...` script piped
                // through `nc`, say) still deserves its answer.
                std::string line;
                size_t      pos;
                while((pos = conn.inbuf.find('\n')) != std::string::npos) {
                    line = conn.inbuf.substr(0, pos);
                    conn.inbuf.erase(0, pos + 1);
                    handleRequestLine(conn, line);
                }
                tryFlush(conn);
                connections_.remove_if([&](std::unique_ptr<Connection>& c) {
                    if(c.get() != &conn) { return false; }
                    closeConnection(*c);
                    return true;
                });
                return;
            }
            // n < 0
            if(errno == EAGAIN || errno == EWOULDBLOCK) { break; }
            if(errno == EINTR) { continue; }
            connections_.remove_if([&](std::unique_ptr<Connection>& c) {
                if(c.get() != &conn) { return false; }
                closeConnection(*c);
                return true;
            });
            return;
        }

        size_t pos;
        while((pos = conn.inbuf.find('\n')) != std::string::npos) {
            std::string line = conn.inbuf.substr(0, pos);
            conn.inbuf.erase(0, pos + 1);
            handleRequestLine(conn, line);
        }
        tryFlush(conn);
    }

    if(mask & WL_EVENT_WRITABLE) { tryFlush(conn); }
}

void IpcServer::tryFlush(Connection& conn) {
    while(!conn.outbuf.empty()) {
        ssize_t n = write(conn.fd, conn.outbuf.data(), conn.outbuf.size());
        if(n > 0) {
            conn.outbuf.erase(0, static_cast<size_t>(n));
            continue;
        }
        if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { break; }
        if(n < 0 && errno == EINTR) { continue; }
        // A real write error (EPIPE, ECONNRESET, ...) -- the read side
        // of handleConnectionReady will notice the closed fd on its own
        // next dispatch (HANGUP/ERROR or a 0-byte read); nothing to do
        // here but stop trying to write and drop what's buffered.
        conn.outbuf.clear();
        break;
    }

    bool need_writable = !conn.outbuf.empty();
    if(need_writable != conn.watching_writable && conn.source) {
        uint32_t mask =
            WL_EVENT_READABLE | (need_writable ? WL_EVENT_WRITABLE : 0);
        wl_event_source_fd_update(conn.source, mask);
        conn.watching_writable = need_writable;
    }
}

void IpcServer::handleRequestLine(Connection& conn, const std::string& line) {
    std::vector<std::string> tok = splitTokens(line);
    if(tok.empty()) { return; }  // blank line -- nothing owed back

    const std::string& cmd   = tok[0];
    auto               reply = [&](const std::string& body) {
        conn.outbuf += body;
        conn.outbuf += '\n';
    };
    auto ok      = [&]() { reply("ok"); };
    auto errorOf = [&](const std::string& why) { reply("error: " + why); };

    if(cmd == "get_outputs") {
        std::string body  = "[";
        bool        first = true;
        for(auto& out : server_.outputs) {
            if(!first) { body += ","; }
            first = false;
            body += outputSnapshot(server_, *out);
        }
        body += "]";
        reply(body);
        return;
    }

    if(cmd == "get_clients") {
        std::string body  = "[";
        bool        first = true;
        for(auto& v : server_.views) {
            if(!v->mapped || v->unmanaged) { continue; }
            if(!first) { body += ","; }
            first = false;
            body += clientSnapshot(server_, *v);
        }
        body += "]";
        reply(body);
        return;
    }

    if(cmd == "get_tags") {
        if(tok.size() != 2) {
            errorOf("usage: get_tags <output>");
            return;
        }
        for(auto& out : server_.outputs) {
            if(tok[1] == out->wlr_output->name) {
                reply("{\"tagset\":" + std::to_string(out->tagset) + "}");
                return;
            }
        }
        errorOf("no such output: " + tok[1]);
        return;
    }

    if(cmd == "focus_tag") {
        uint32_t n = 0;
        if(tok.size() < 2 || !parseUInt(tok[1], n) || n < 1 ||
           n > static_cast<uint32_t>(cfg::kTagCount)) {
            errorOf("usage: focus_tag <n> [output] (1 <= n <= " +
                    std::to_string(cfg::kTagCount) + ")");
            return;
        }
        Output* out = nullptr;
        if(tok.size() >= 3) {
            for(auto& o : server_.outputs) {
                if(tok[2] == o->wlr_output->name) { out = o.get(); }
            }
            if(!out) {
                errorOf("no such output: " + tok[2]);
                return;
            }
        } else {
            out = server_.focused_output;
        }
        if(!out) {
            errorOf("no focused output and none named");
            return;
        }
        out->setTagset(1u << (n - 1));
        ok();
        return;
    }

    if(cmd == "set_tag") {
        uint32_t id = 0, n = 0;
        if(tok.size() != 3 || !parseUInt(tok[1], id) || !parseUInt(tok[2], n) ||
           n < 1 || n > static_cast<uint32_t>(cfg::kTagCount)) {
            errorOf("usage: set_tag <client_id> <n>");
            return;
        }
        View* view = nullptr;
        for(auto& v : server_.views) {
            if(v->id == id) { view = v.get(); }
        }
        if(!view) {
            errorOf("no such client: " + tok[1]);
            return;
        }
        view->setTags(1u << (n - 1));
        ok();
        return;
    }

    if(cmd == "close") {
        uint32_t id = 0;
        if(tok.size() != 2 || !parseUInt(tok[1], id)) {
            errorOf("usage: close <client_id>");
            return;
        }
        View* view = nullptr;
        for(auto& v : server_.views) {
            if(v->id == id) { view = v.get(); }
        }
        if(!view) {
            errorOf("no such client: " + tok[1]);
            return;
        }
        view->close();
        ok();
        return;
    }

    if(cmd == "close_all_on_tag") {
        uint32_t n = 0;
        if(tok.size() != 2 || !parseUInt(tok[1], n) || n < 1 ||
           n > static_cast<uint32_t>(cfg::kTagCount)) {
            errorOf("usage: close_all_on_tag <n>");
            return;
        }
        server_.closeOnTag(1u << (n - 1));
        ok();
        return;
    }

    if(cmd == "reload") {
        server_.reloadConfig();
        ok();
        return;
    }

    if(cmd == "subscribe") {
        if(tok.size() != 2) {
            errorOf("usage: subscribe <client,output,tag|*>");
            return;
        }
        bool wildcard = (tok[1] == "*");
        conn.sub_client =
            wildcard || tok[1].find("client") != std::string::npos;
        conn.sub_output =
            wildcard || tok[1].find("output") != std::string::npos;
        conn.sub_tag = wildcard || tok[1].find("tag") != std::string::npos;
        ok();
        return;
    }

    errorOf("unknown command: " + cmd);
}

void IpcServer::broadcast(const std::string& category,
                          const std::string& json_line) {
    for(auto& conn : connections_) {
        bool wants = (category == "client" && conn->sub_client) ||
                     (category == "output" && conn->sub_output) ||
                     (category == "tag" && conn->sub_tag);
        if(!wants) { continue; }
        conn->outbuf += json_line;
        conn->outbuf += '\n';
        tryFlush(*conn);
    }
}

void IpcServer::onClientEvent(const std::string& event, View* view) {
    std::ostringstream os;
    os << "{\"event\":\"" << jsonEscape(event) << "\""
       << ",\"id\":" << view->id << ",\"app_id\":\"" << jsonEscape(view->app_id)
       << "\""
       << ",\"title\":\"" << jsonEscape(view->title) << "\""
       << "}";
    broadcast("client", os.str());
}

void IpcServer::onOutputEvent(const std::string& event,
                              const std::string& output_name) {
    std::ostringstream os;
    os << "{\"event\":\"" << jsonEscape(event) << "\""
       << ",\"output\":\"" << jsonEscape(output_name) << "\""
       << "}";
    broadcast("output", os.str());
}

void IpcServer::onTagChange(const std::string& output_name,
                            uint32_t           new_tagset) {
    std::ostringstream os;
    os << "{\"event\":\"tag::change\""
       << ",\"output\":\"" << jsonEscape(output_name) << "\""
       << ",\"tagset\":" << new_tagset << "}";
    broadcast("tag", os.str());
}
