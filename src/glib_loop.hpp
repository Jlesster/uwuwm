#pragma once

extern "C" {
#include <wayland-server-core.h>
}

#include <glib.h>

// Lets Server::run() drive its wl_event_loop from inside a GMainLoop
// instead of wl_display_run(). This is the same trick mutter uses to
// run libwayland's event loop underneath a GLib-based compositor --
// wrap the loop's single pollable fd in a custom GSource, and let
// GLib's own poll()/epoll iteration dispatch it alongside everything
// else attached to the default GMainContext.
//
// Why this exists at all: embedding Qt/QtQuick means Qt's own
// QPAEventDispatcherGlib wants to attach its timers/animations/socket
// notifiers to *some* running GMainContext. Rather than merge two
// independent event loops by hand (polling Qt's fds ourselves, or
// polling wl_event_loop's fd from inside Qt), we let GLib's loop be
// the one true master loop and teach it to also service
// wl_event_loop -- so every existing wl_event_loop_add_timer/
// add_signal/add_fd call already in server.cpp keeps working
// completely unchanged; only *what iterates the loop* changes.
//
// Verified (see the qml-spike/ throwaway spikes this was developed
// against, not just assumed): a QTimer fires correctly under a bare
// g_main_loop_run() with zero calls to QGuiApplication::exec()
// anywhere in the process, and a wl_event_loop_add_timer callback
// fires correctly when the loop is driven purely through this
// GSource, with no wl_display_run() call anywhere either.
class GlibEventLoop {
public:
    // `loop` must outlive this object -- same lifetime relationship
    // Listener<T> has with the wl_signal it's connected to. Typically
    // called once in Server::setup() with wl_display_get_event_loop(display).
    explicit GlibEventLoop(wl_event_loop* loop) {
        source_           = g_source_new(&kFuncs, sizeof(SourceData));
        auto* data        = reinterpret_cast<SourceData*>(source_);
        data->loop        = loop;
        data->pfd.fd      = wl_event_loop_get_fd(loop);
        data->pfd.events  = G_IO_IN | G_IO_ERR;
        data->pfd.revents = 0;
        g_source_add_poll(source_, &data->pfd);
        g_source_set_priority(source_, G_PRIORITY_DEFAULT);
        g_source_attach(source_,
                        /*context=*/nullptr);  // thread-default context
    }

    ~GlibEventLoop() {
        g_source_destroy(source_);
        g_source_unref(source_);
    }

    GlibEventLoop(const GlibEventLoop&)            = delete;
    GlibEventLoop& operator=(const GlibEventLoop&) = delete;

private:
    struct SourceData {
        GSource
            source;  // must be first member -- GSource "subclassing" convention
        GPollFD        pfd;
        wl_event_loop* loop;
    };

    static gboolean prepare(GSource* base, gint* timeout) {
        auto* data = reinterpret_cast<SourceData*>(base);
        *timeout   = -1;
        // Flush idle sources (wl_event_loop_add_idle) so their pending
        // work happens even on an iteration where the fd itself never
        // goes readable -- mirrors what wl_display_run does internally.
        wl_event_loop_dispatch_idle(data->loop);
        return FALSE;
    }

    static gboolean check(GSource* base) {
        auto* data = reinterpret_cast<SourceData*>(base);
        return data->pfd.revents != 0;
    }

    static gboolean dispatch(GSource* base, GSourceFunc, gpointer) {
        auto* data = reinterpret_cast<SourceData*>(base);
        wl_event_loop_dispatch(data->loop, 0);
        return G_SOURCE_CONTINUE;
    }

    static inline GSourceFuncs kFuncs = {
        .prepare  = prepare,
        .check    = check,
        .dispatch = dispatch,
        .finalize = nullptr,
    };

    GSource* source_ = nullptr;
};
