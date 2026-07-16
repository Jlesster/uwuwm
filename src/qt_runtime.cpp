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
    impl->argv_ptrs.push_back(
        nullptr);  // argv[] is NULL-terminated by convention

    // "offscreen" QPA platform: uwuwm never creates a real on-screen
    // QWindow anywhere in this pipeline, so there's no real display
    // server for Qt to talk to on a bare TTY/DRM session. On a nested
    // dev session (WAYLAND_DISPLAY/DISPLAY already set) this is
    // harmless either way. Not overridden if the environment already
    // sets QT_QPA_PLATFORM, so a developer can force "eglfs"/"xcb" for
    // testing without a rebuild.
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    // Root-caused the black-screen/hang: "offscreen" genuinely cannot
    // hand out a working GL context -- confirmed directly (small repro
    // outside this codebase) that QOpenGLContext::create() returns
    // false under it, with no real display/DRM/EGL device backing it
    // at all. qml_bar.cpp's old FBO-based pipeline didn't check that
    // return value, so it went on to call QQuickRenderControl with a
    // dead context; every driver reacts differently to the GL calls
    // that follow -- an assert-abort on a debug Qt build, but with
    // asserts compiled out (the normal case for a distro-packaged Qt,
    // e.g. on Arch/NixOS) it's undefined behaviour, and the specific
    // way this showed up here was as an indefinite hang inside the
    // render pipeline -- which is exactly the black screen + hang
    // being chased.
    //
    // Forcing the QSG "software" scenegraph adaptation sidesteps the
    // problem instead of working around it: QQuickRenderControl then
    // rasterizes on the CPU and QQuickWindow::grabWindow() hands back
    // a plain QImage, so nothing here ever needs a real GL/EGL context
    // at all (see qml_bar.cpp's Impl, which no longer touches
    // QOpenGLContext/QOffscreenSurface/QOpenGLFramebufferObject).
    // Costs some CPU for a status bar's worth of rects/text, which is
    // a non-issue; sharing wlroots' own EGL context with Qt (raw EGL
    // native handle, GPU-accelerated) is the real long-term fix if
    // that ever changes, but it's a materially bigger change than this
    // bug needs. Same not-overridden-if-already-set convention as
    // QT_QPA_PLATFORM above.
    if(qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND")) {
        qputenv("QT_QUICK_BACKEND", "software");
    }

    impl->app =
        std::make_unique<QGuiApplication>(impl->argc, impl->argv_ptrs.data());
    impl->engine = std::make_unique<QQmlEngine>();
}

QtRuntime::~QtRuntime() = default;

QQmlEngine* QtRuntime::engine() { return impl->engine.get(); }
