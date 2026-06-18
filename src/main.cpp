#include "MainWindow.h"

#include <gtk/gtk.h>

namespace {

MainWindow *g_mainWindow = nullptr;

void onActivate(GtkApplication *app, gpointer) {
    if (!g_mainWindow)
        g_mainWindow = new MainWindow(app);
    g_mainWindow->present();
}

void onShutdown(GApplication *, gpointer) {
    delete g_mainWindow;
    g_mainWindow = nullptr;
}

} // namespace

int main(int argc, char **argv) {
    GtkApplication *app =
        gtk_application_new("org.scrotocol.Scrotocol", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(onActivate), nullptr);
    g_signal_connect(app, "shutdown", G_CALLBACK(onShutdown), nullptr);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
