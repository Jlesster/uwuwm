#pragma once

#include <memory>

class QQmlEngine;

// Owns the one QGuiApplication a process may have (Qt's own
// requirement) plus one shared QQmlEngine every QmlBar (qml_bar.hpp)
// builds its QQmlComponents against. Constructed once in
// Server::setup(), before rc.lua even loads -- see that call site's
// comment for why it has to be that early.
//
// Deliberately header-free of any real Qt include: QGuiApplication.h
// and QQmlEngine.h (transitively) bring in Qt's `signals`/`slots`/
// `emit` macros, a real, documented source of collisions with
// unrelated code, and server.hpp/server.cpp -- which both need to
// reference this type -- are two of the last places in this codebase
// that should be exposed to that risk. qt_runtime.cpp is where the
// real Qt headers live; everything else just sees this pimpl.
class QtRuntime {
public:
    // Takes its own persistent copy of argc/argv rather than storing
    // the caller's pointers: QGuiApplication's documented contract is
    // that the argc/argv it's constructed with must stay valid for the
    // application's *entire* lifetime (Qt may still reference them
    // later, not just during construction) -- and argc/argv as passed
    // down through Server::run() -> Server::setup() only live on those
    // functions' stack frames, which are long gone by the time
    // QGuiApplication might need them again. Easy to get wrong exactly
    // because it usually doesn't crash immediately; getting this right
    // by construction matters more than usual here.
    QtRuntime(int argc, char** argv);
    ~QtRuntime();

    QtRuntime(const QtRuntime&)            = delete;
    QtRuntime& operator=(const QtRuntime&) = delete;

    // The one QQmlEngine every QmlBar shares -- see server.hpp's
    // qt_runtime member comment for why it's shared rather than
    // per-bar.
    QQmlEngine* engine();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
