#pragma once

#include <functional>
#include <string>

enum class CaptureMode { FullScreen, ActiveWindow, Region };

// Result delivered on the main loop once scrot has exited.
// On cancellation (e.g. Escape during region select) `success` is false
// and `errorMessage` is empty -- callers should treat that as a silent no-op.
using CaptureCallback =
    std::function<void(bool success, const std::string &filePath, const std::string &errorMessage)>;

class Capture {
public:
    // Fires `callback` on the GLib main loop when scrot finishes.
    // `delaySeconds` is handled by the caller (countdown UI) -- by the time
    // this is invoked the capture should happen immediately.
    static void run(CaptureMode mode, CaptureCallback callback);
};
