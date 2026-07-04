#pragma once

extern "C" {
#include <wayland-server-core.h>
}

#include <functional>

// Wraps the wl_signal + wl_listener pattern that's the backbone of every
// wlroots callback (new_output, new_toplevel, map, unmap, destroy, ...).
//
// In raw C you embed a `struct wl_listener` in your own struct and recover
// it in the notify callback with `wl_container_of`. Here we do the same
// thing, but the recovery is mechanical and the callback is a std::function
// instead of a free function, so call sites read as ordinary C++ lambdas
// instead of "find the struct, find the macro, find the offset".
//
// Cost: one extra indirection (calling through std::function) per signal
// fire. That's irrelevant for new_output/new_toplevel/map/unmap/destroy --
// all low frequency. It is *not* used for the per-output frame callback;
// see output.cpp, which uses a plain free-function notify directly to avoid
// paying this on the hot path, per the doc's §2.1 guidance.
template <typename T>
class Listener {
public:
	Listener() { wl.notify = &Listener::trampoline; }

	Listener(const Listener &) = delete;
	Listener &operator=(const Listener &) = delete;

	~Listener() {
		if (wl.link.next != nullptr) {
			wl_list_remove(&wl.link);
		}
	}

	// Registers `cb` against `signal`. `cb` receives the signal's event data
	// already cast to T* (or nullptr for signals that pass no data, like
	// most `destroy` events -- callers must not dereference in that case).
	void connect(wl_signal *signal, std::function<void(T *)> cb) {
		if (wl.link.next != nullptr) {
			wl_list_remove(&wl.link);
		}
		callback = std::move(cb);
		wl_signal_add(signal, &wl);
	}

	void disconnect() {
		if (wl.link.next != nullptr) {
			wl_list_remove(&wl.link);
			wl.link.next = nullptr;
			wl.link.prev = nullptr;
		}
	}

	wl_listener wl{};

private:
	static void trampoline(wl_listener *listener, void *data) {
		// Since `wl` is the first member of Listener and the class has no virtual
		// functions, the address of the wl_listener is the address of the
		// Listener object itself.
		auto *self = reinterpret_cast<Listener *>(listener);
		if (self->callback) {
			self->callback(static_cast<T *>(data));
		}
	}

	std::function<void(T *)> callback;
};

// Some signals (most `destroy` events, `request_*` with no payload) pass no
// meaningful data pointer. Use this alias for clarity at call sites.
using VoidListener = Listener<void>;
