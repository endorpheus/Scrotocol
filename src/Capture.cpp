#include "Capture.h"

#include <gio/gio.h>
#include <glib.h>

#include <vector>

namespace {

struct CaptureContext {
    std::string filePath;
    CaptureCallback callback;
};

std::string makeTempPath() {
    g_autofree gchar *name = g_strdup_printf(
        "scrotocol-%ld.png", static_cast<long>(g_get_real_time()));
    g_autofree gchar *path = g_build_filename(g_get_tmp_dir(), name, nullptr);
    return std::string(path);
}

void onProcessExit(GObject *source, GAsyncResult *result, gpointer userData) {
    auto *ctx = static_cast<CaptureContext *>(userData);
    GSubprocess *proc = G_SUBPROCESS(source);

    GError *error = nullptr;
    gboolean finished = g_subprocess_wait_check_finish(proc, result, &error);

    bool fileExists = g_file_test(ctx->filePath.c_str(), G_FILE_TEST_EXISTS);

    if (finished && fileExists) {
        ctx->callback(true, ctx->filePath, "");
    } else if (!fileExists) {
        // Most likely the user cancelled an interactive selection (Escape).
        g_clear_error(&error);
        ctx->callback(false, "", "");
    } else {
        std::string message = error ? error->message : "scrot exited with an error";
        g_clear_error(&error);
        ctx->callback(false, "", message);
    }

    delete ctx;
}

} // namespace

void Capture::run(CaptureMode mode, CaptureCallback callback) {
    std::string filePath = makeTempPath();

    std::vector<std::string> args = {"scrot", "-o", "-z"};
    switch (mode) {
    case CaptureMode::FullScreen:
        break;
    case CaptureMode::ActiveWindow:
        args.push_back("-u");
        args.push_back("-b");
        break;
    case CaptureMode::Region:
        args.push_back("-s");
        args.push_back("-f");
        break;
    }
    args.push_back("-F");
    args.push_back(filePath);

    std::vector<const char *> argv;
    argv.reserve(args.size() + 1);
    for (const auto &a : args)
        argv.push_back(a.c_str());
    argv.push_back(nullptr);

    GError *error = nullptr;
    GSubprocess *proc = g_subprocess_newv(
        argv.data(), static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                                     G_SUBPROCESS_FLAGS_STDERR_SILENCE),
        &error);

    if (!proc) {
        std::string message = error ? error->message : "failed to launch scrot";
        g_clear_error(&error);
        callback(false, "", message);
        return;
    }

    auto *ctx = new CaptureContext{filePath, std::move(callback)};
    g_subprocess_wait_check_async(proc, nullptr, onProcessExit, ctx);
    g_object_unref(proc);
}
