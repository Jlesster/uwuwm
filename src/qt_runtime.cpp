#include "qt_runtime.hpp"

#include <QGuiApplication>
#include <QQmlEngine>

#include <string>
#include <vector>

struct QtRuntime::Impl {
    // Owned, persistent copies -- see qt_runtime.hpp's constructor
    // comment for why this can't just be the caller's argc/argv
    // pointers. argv_ptrs points into argv_storage's owned strings and
    // is what actually gets handed to QGuiApplication; it must outlive
    // `app` (declared after it here, so it's destroyed after `app` --
    // reverse member-destruction order -- though in practice
    // QGuiApplication doesn't hold onto argv past its own destruction
    // either way).
    std::vector<std::string> argv_storage;
    std::vector<char*>       argv_ptrs;
    int                      argc = 0;

    std::unique_ptr<QGuiApplication> app;
    std::unique_ptr<QQmlEngine>      engine;
};

QtRuntime::QtRuntime(int argc, char** argv) : impl(std::make_unique<Impl>()) {
    impl->argc = argc;
    impl->argv_storage.reserve(static_cast<size_t>(argc));
    for(int i = 0; i < argc; i++) { impl->argv_storage.emplace_back(argv[i]); }
    impl->argv_ptrs.reserve(static_cast<size_t>(argc) + 1);
    for(auto& s : impl->argv_storage) { impl->argv_ptrs.push_back(s.data()); }
    impl->argv_ptrs.push_back(nullptr);  // argv[] is NULL-terminated by convention

    // "offscreen" QPA platform: uwuwm never creates a real on-screen
    // QWindow anywhere in this pipeline (QmlBar renders into an FBO,
    // see qml_bar.cpp), so there's no real display server for Qt to
    // talk to on a bare TTY/DRM session. On a nested dev session
    // (WAYLAND_DISPLAY/DISPLAY already set) this is harmless either
    // way. NOTE, found the hard way while spiking this: "offscreen"
    // could NOT create a real GL context in that sandbox testing --
    // only a real display (there, Xvfb + "xcb") could. On uwuwm's
    // actual bare-TTY/DRM target this whole assumption needs
    // revisiting -- likely "eglfs", or a raw EGL context shared against
    // wlroots' own DRM render node (the milestone-3 design, parked
    // until CPU-readback profiling actually calls for it). Not
    // overridden if the environment already sets QT_QPA_PLATFORM, so a
    // developer can force "eglfs"/"xcb" for testing without a rebuild.
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    impl->app = std::make_unique<QGuiApplication>(impl->argc, impl->argv_ptrs.data());
    impl->engine = std::make_unique<QQmlEngine>();
}

QtRuntime::~QtRuntime() = default;

QQmlEngine* QtRuntime::engine() { return impl->engine.get(); }
