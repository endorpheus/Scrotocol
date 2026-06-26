#include "MainWindow.h"

#include "Config.h"

#include <glib/gstdio.h>
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <vector>

namespace {

const char *formatForPath(const std::string &path) {
    if (g_str_has_suffix(path.c_str(), ".jpg") || g_str_has_suffix(path.c_str(), ".jpeg"))
        return "jpeg";
    if (g_str_has_suffix(path.c_str(), ".bmp"))
        return "bmp";
    return "png";
}

struct CountdownCtx {
    MainWindow *self;
    int remaining;
    std::function<void()> done;
};

gboolean countdownTick(gpointer data) {
    auto *ctx = static_cast<CountdownCtx *>(data);
    if (ctx->remaining <= 0) {
        auto done = std::move(ctx->done);
        delete ctx;
        done();
        return G_SOURCE_REMOVE;
    }

    g_autofree gchar *msg = g_strdup_printf("Capturing in %d…", ctx->remaining);
    ctx->self->setStatus(msg);
    ctx->remaining--;
    g_timeout_add_seconds(1, countdownTick, ctx);
    return G_SOURCE_REMOVE;
}

struct DelayedCaptureCtx {
    MainWindow *self;
    CaptureMode mode;
};

gboolean delayedCaptureTick(gpointer data) {
    auto *ctx = static_cast<DelayedCaptureCtx *>(data);
    ctx->self->doCapture(ctx->mode);
    delete ctx;
    return G_SOURCE_REMOVE;
}

struct SaveCtx {
    MainWindow *self;
    GdkPixbuf *pixbuf;
};

struct RowDragCtx {
    char *path;
    bool shift_only;
};

void freeRowDragCtx(gpointer data) {
    auto *ctx = static_cast<RowDragCtx *>(data);
    g_free(ctx->path);
    delete ctx;
}

void dragCtxNotify(gpointer data, GClosure *) {
    freeRowDragCtx(data);
}

GdkContentProvider *onDragPrepare(GtkDragSource *src, double, double, gpointer ud) {
    auto *ctx = static_cast<RowDragCtx *>(ud);
    if (ctx->shift_only) {
        GdkEvent *ev = gtk_gesture_get_last_event(GTK_GESTURE(src), nullptr);
        if (!ev || !(gdk_event_get_modifier_state(ev) & GDK_SHIFT_MASK))
            return nullptr;
    }
    gchar *uri = g_strdup_printf("file://%s\r\n", ctx->path);
    GBytes *bytes = g_bytes_new_take(uri, strlen(uri));
    GdkContentProvider *prov = gdk_content_provider_new_for_bytes("text/uri-list", bytes);
    g_bytes_unref(bytes);
    return prov;
}

void onRowRightClicked(GtkGestureClick *, int, double x, double y, gpointer popover) {
    GdkRectangle rect = {static_cast<int>(x), static_cast<int>(y), 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    gtk_popover_popup(GTK_POPOVER(popover));
}

} // namespace

namespace {

// Arc-Dark (and a few other GTK3 themes) give menu separators the same
// background color as the menu itself, making them invisible. Append a
// minimal override to the user's gtk-3.0/gtk.css if it isn't already there.
// This affects every GTK3 app on the system, but only the specific
// property that was wrong to begin with.
void ensureGtk3SeparatorFix() {
    const char *kMarker = "/* scrotocol-separator-fix */";
    const char *kRule   = "\n/* scrotocol-separator-fix */\n"
                          "menu separator, .menu separator { background-color: #4f5461; }\n";

    g_autofree gchar *dir  = g_build_filename(g_get_user_config_dir(), "gtk-3.0", nullptr);
    g_autofree gchar *path = g_build_filename(dir, "gtk.css", nullptr);

    gchar *existing = nullptr;
    g_file_get_contents(path, &existing, nullptr, nullptr);
    if (existing && strstr(existing, kMarker)) {
        g_free(existing);
        return;
    }
    g_free(existing);

    g_mkdir_with_parents(dir, 0755);

    FILE *f = fopen(path, "a");
    if (f) {
        fputs(kRule, f);
        fclose(f);
    }
}

} // namespace

MainWindow::MainWindow(GtkApplication *app) : app_(app) {
    ensureGtk3SeparatorFix();
    buildUi();
    tray_ = std::make_unique<TrayIcon>("accessories-screenshot-tool",
        [this](TrayAction action) {
            switch (action) {
                case TrayAction::FullScreen:
                    runCountdown(0, [this]() { beginCapture(CaptureMode::FullScreen); });
                    break;
                case TrayAction::Region:
                    runCountdown(0, [this]() { beginCapture(CaptureMode::Region); });
                    break;
                case TrayAction::Window:
                    runCountdown(0, [this]() { beginCapture(CaptureMode::ActiveWindow); });
                    break;
                case TrayAction::Timer: {
                    int delay = Config::instance().defaultDelaySeconds();
                    runCountdown(delay, [this]() { beginCapture(CaptureMode::FullScreen); });
                    break;
                }
                case TrayAction::Settings:
                case TrayAction::Show:
                    present();
                    break;
                case TrayAction::Quit:
                    g_application_quit(G_APPLICATION(app_));
                    break;
            }
        });
    refreshHistory();
}

MainWindow::~MainWindow() = default;

void MainWindow::present() { gtk_window_present(GTK_WINDOW(window_)); }

void MainWindow::setStatus(const std::string &text) {
    gtk_label_set_text(GTK_LABEL(status_label_), text.c_str());
}

void MainWindow::buildUi() {
    window_ = gtk_application_window_new(app_);
    gtk_window_set_title(GTK_WINDOW(window_), "Scrotocol");
    gtk_window_set_default_size(GTK_WINDOW(window_), 980, 640);
    gtk_window_set_icon_name(GTK_WINDOW(window_), "accessories-screenshot-tool");

    GtkWidget *headerBar = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(window_), headerBar);

    GtkWidget *btnFull = gtk_button_new_with_label("Full Screen");
    GtkWidget *btnWindow = gtk_button_new_with_label("Active Window");
    GtkWidget *btnRegion = gtk_button_new_with_label("Select Region");
    g_object_set_data(G_OBJECT(btnFull), "scrotocol-mode",
                       GINT_TO_POINTER(static_cast<int>(CaptureMode::FullScreen)));
    g_object_set_data(G_OBJECT(btnWindow), "scrotocol-mode",
                       GINT_TO_POINTER(static_cast<int>(CaptureMode::ActiveWindow)));
    g_object_set_data(G_OBJECT(btnRegion), "scrotocol-mode",
                       GINT_TO_POINTER(static_cast<int>(CaptureMode::Region)));
    g_signal_connect(btnFull, "clicked", G_CALLBACK(onCaptureButtonClickedTrampoline), this);
    g_signal_connect(btnWindow, "clicked", G_CALLBACK(onCaptureButtonClickedTrampoline), this);
    g_signal_connect(btnRegion, "clicked", G_CALLBACK(onCaptureButtonClickedTrampoline), this);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(headerBar), btnFull);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(headerBar), btnWindow);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(headerBar), btnRegion);

    GtkWidget *delayBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(delayBox), gtk_label_new("Delay:"));
    delay_spin_ = gtk_spin_button_new_with_range(0, 10, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(delay_spin_), Config::instance().defaultDelaySeconds());
    g_signal_connect(delay_spin_, "value-changed", G_CALLBACK(onDelayChangedTrampoline), this);
    gtk_box_append(GTK_BOX(delayBox), delay_spin_);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(headerBar), delayBox);

    GtkWidget *folderBtn = gtk_button_new_from_icon_name("folder-open-symbolic");
    gtk_widget_set_tooltip_text(folderBtn, "Open history folder");
    g_signal_connect(folderBtn, "clicked", G_CALLBACK(onOpenFolderClickedTrampoline), this);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(headerBar), folderBtn);

    GtkWidget *minimizeBtn = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_set_tooltip_text(minimizeBtn, "Minimize to tray");
    g_signal_connect(minimizeBtn, "clicked", G_CALLBACK(onMinimizeClickedTrampoline), this);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(headerBar), minimizeBtn);

    GtkWidget *settingsPopover = gtk_popover_new();
    GtkWidget *settingsBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(settingsBox, 12);
    gtk_widget_set_margin_end(settingsBox, 12);
    gtk_widget_set_margin_top(settingsBox, 8);
    gtk_widget_set_margin_bottom(settingsBox, 8);
    GtkWidget *minimizeCheck = gtk_check_button_new_with_label("Minimize before capture");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(minimizeCheck),
                                 Config::instance().minimizeBeforeCapture());
    g_signal_connect(minimizeCheck, "notify::active",
                     G_CALLBACK(onMinimizeBeforeCaptureToggledTrampoline), nullptr);
    gtk_box_append(GTK_BOX(settingsBox), minimizeCheck);

    GtkWidget *taskbarIconCheck = gtk_check_button_new_with_label("Show last capture as taskbar icon");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(taskbarIconCheck),
                                 Config::instance().taskbarIconUseCapture());
    g_signal_connect(taskbarIconCheck, "notify::active",
                     G_CALLBACK(onTaskbarIconToggledTrampoline), this);
    gtk_box_append(GTK_BOX(settingsBox), taskbarIconCheck);
    gtk_popover_set_child(GTK_POPOVER(settingsPopover), settingsBox);
    GtkWidget *settingsBtn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(settingsBtn), "preferences-system-symbolic");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(settingsBtn), settingsPopover);
    gtk_widget_set_tooltip_text(settingsBtn, "Settings");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(headerBar), settingsBtn);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_hexpand(paned, TRUE);
    gtk_widget_set_vexpand(paned, TRUE);

    // --- Left: preview + toolbar ---
    GtkWidget *leftBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(leftBox, 6);
    gtk_widget_set_margin_end(leftBox, 6);
    gtk_widget_set_margin_top(leftBox, 6);
    gtk_widget_set_margin_bottom(leftBox, 6);

    gtk_widget_set_size_request(crop_overlay_.widget(), 480, 320);
    gtk_box_append(GTK_BOX(leftBox), crop_overlay_.widget());

    GtkWidget *toolbarBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *applyCropBtn = gtk_button_new_with_label("Apply Crop");
    GtkWidget *resetCropBtn = gtk_button_new_with_label("Reset");
    GtkWidget *copyBtn = gtk_button_new_with_label("Copy to Clipboard");
    GtkWidget *saveAsBtn = gtk_button_new_with_label("Save As…");
    g_signal_connect(applyCropBtn, "clicked", G_CALLBACK(onApplyCropClickedTrampoline), this);
    g_signal_connect(resetCropBtn, "clicked", G_CALLBACK(onResetCropClickedTrampoline), this);
    g_signal_connect(copyBtn, "clicked", G_CALLBACK(onCopyClickedTrampoline), this);
    g_signal_connect(saveAsBtn, "clicked", G_CALLBACK(onSaveAsClickedTrampoline), this);
    gtk_box_append(GTK_BOX(toolbarBox), applyCropBtn);
    gtk_box_append(GTK_BOX(toolbarBox), resetCropBtn);
    gtk_box_append(GTK_BOX(toolbarBox), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(toolbarBox), copyBtn);
    gtk_box_append(GTK_BOX(toolbarBox), saveAsBtn);

    status_label_ = gtk_label_new("Ready");
    gtk_widget_set_hexpand(status_label_, TRUE);
    gtk_label_set_xalign(GTK_LABEL(status_label_), 1.0);
    gtk_widget_add_css_class(status_label_, "dim-label");
    gtk_box_append(GTK_BOX(toolbarBox), status_label_);

    gtk_box_append(GTK_BOX(leftBox), toolbarBox);
    gtk_paned_set_start_child(GTK_PANED(paned), leftBox);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);

    // --- Right: history sidebar ---
    GtkWidget *rightBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(rightBox, 240, -1);
    gtk_widget_set_margin_end(rightBox, 6);
    gtk_widget_set_margin_top(rightBox, 6);
    gtk_widget_set_margin_bottom(rightBox, 6);

    GtkWidget *historyLabel = gtk_label_new("History");
    gtk_widget_add_css_class(historyLabel, "heading");
    gtk_label_set_xalign(GTK_LABEL(historyLabel), 0);
    gtk_box_append(GTK_BOX(rightBox), historyLabel);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    history_list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(history_list_), GTK_SELECTION_NONE);
    g_signal_connect(history_list_, "row-activated", G_CALLBACK(onHistoryRowActivatedTrampoline),
                      this);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), history_list_);
    gtk_box_append(GTK_BOX(rightBox), scrolled);

    gtk_paned_set_end_child(GTK_PANED(paned), rightBox);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);

    gtk_window_set_child(GTK_WINDOW(window_), paned);

    // Openbox (and many WMs) auto-focus newly mapped windows. Without this,
    // initial keyboard focus lands on the first header-bar button ("Full
    // Screen"), so a stray Enter/Space keystroke meant for another window
    // would fire a real capture. Land focus on the harmless delay spinner
    // instead.
    gtk_widget_grab_focus(delay_spin_);
}

void MainWindow::onCaptureButtonClicked(CaptureMode mode) {
    int delay = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(delay_spin_));
    runCountdown(delay, [this, mode]() { beginCapture(mode); });
}

void MainWindow::runCountdown(int seconds, std::function<void()> done) {
    if (seconds <= 0) {
        done();
        return;
    }
    auto *ctx = new CountdownCtx{this, seconds, std::move(done)};
    countdownTick(ctx);
}

void MainWindow::beginCapture(CaptureMode mode) {
    setStatus("Capturing…");
    if (Config::instance().minimizeBeforeCapture())
        gtk_widget_set_visible(window_, FALSE);
    auto *ctx = new DelayedCaptureCtx{this, mode};
    g_timeout_add(150, delayedCaptureTick, ctx);
}

void MainWindow::doCapture(CaptureMode mode) {
    Capture::run(mode, [this](bool success, const std::string &path, const std::string &error) {
        onCaptureFinished(success, path, error);
    });
}

void MainWindow::onCaptureFinished(bool success, const std::string &path,
                                    const std::string &error) {
    gtk_widget_set_visible(window_, TRUE);
    gtk_window_present(GTK_WINDOW(window_));

    if (!success) {
        setStatus(error.empty() ? "Capture cancelled" : "Capture failed: " + error);
        return;
    }

    GError *gerror = nullptr;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path.c_str(), &gerror);
    g_remove(path.c_str());

    if (!pixbuf) {
        setStatus(std::string("Failed to load capture: ") + (gerror ? gerror->message : ""));
        g_clear_error(&gerror);
        return;
    }

    crop_overlay_.setImage(pixbuf);
    updateTaskbarIcon(pixbuf);
    std::string savedPath = history_store_.saveCapture(pixbuf);
    g_object_unref(pixbuf);

    if (!savedPath.empty()) {
        setStatus("Saved to history");
        refreshHistory();
    } else {
        setStatus("Captured (history save failed)");
    }
}

void MainWindow::onApplyCropClicked() {
    if (!crop_overlay_.hasSelection()) {
        setStatus("Draw a selection on the preview first");
        return;
    }
    crop_overlay_.applyCrop();
    setStatus("Crop applied");
}

void MainWindow::onResetCropClicked() {
    crop_overlay_.resetCrop();
    setStatus("Crop reset");
}

void MainWindow::onCopyClicked() {
    GdkPixbuf *img = crop_overlay_.image();
    if (!img) {
        setStatus("Nothing to copy yet");
        return;
    }
    GdkTexture *texture = gdk_texture_new_for_pixbuf(img);
    GdkClipboard *clipboard = gdk_display_get_clipboard(gtk_widget_get_display(window_));
    gdk_clipboard_set_texture(clipboard, texture);
    g_object_unref(texture);
    setStatus("Copied to clipboard");
}

void MainWindow::onSaveAsClicked() {
    GdkPixbuf *img = crop_overlay_.image();
    if (!img) {
        setStatus("Nothing to save yet");
        return;
    }

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save Screenshot As");

    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree gchar *stamp = g_date_time_format(now, "%Y-%m-%d_%H-%M-%S");
    g_autofree gchar *defaultName = g_strdup_printf("Scrotocol_%s.png", stamp);
    gtk_file_dialog_set_initial_name(dialog, defaultName);

    GFile *initialFolder = g_file_new_for_path(Config::instance().historyDir().c_str());
    gtk_file_dialog_set_initial_folder(dialog, initialFolder);
    g_object_unref(initialFolder);

    auto *ctx = new SaveCtx{this, GDK_PIXBUF(g_object_ref(img))};
    gtk_file_dialog_save(dialog, GTK_WINDOW(window_), nullptr, onSaveDialogFinished, ctx);
    g_object_unref(dialog);
}

void MainWindow::onSaveDialogFinished(GObject *source, GAsyncResult *result, gpointer userData) {
    auto *ctx = static_cast<SaveCtx *>(userData);

    GError *error = nullptr;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
    if (file) {
        g_autofree gchar *path = g_file_get_path(file);
        GError *saveError = nullptr;
        if (gdk_pixbuf_save(ctx->pixbuf, path, formatForPath(path), &saveError, nullptr)) {
            ctx->self->setStatus("Saved");
        } else {
            ctx->self->setStatus(std::string("Save failed: ") +
                                  (saveError ? saveError->message : ""));
            g_clear_error(&saveError);
        }
        g_object_unref(file);
    } else {
        g_clear_error(&error); // user cancelled
    }

    g_object_unref(ctx->pixbuf);
    delete ctx;
}

void MainWindow::onMinimizeClicked() { gtk_widget_set_visible(window_, FALSE); }

void MainWindow::toggleVisibility() {
    if (gtk_widget_get_visible(window_))
        gtk_widget_set_visible(window_, FALSE);
    else
        gtk_window_present(GTK_WINDOW(window_));
}

void MainWindow::onOpenFolderClicked() {
    g_autofree gchar *uri = g_strdup_printf("file://%s", Config::instance().historyDir().c_str());
    GtkUriLauncher *launcher = gtk_uri_launcher_new(uri);
    gtk_uri_launcher_launch(launcher, GTK_WINDOW(window_), nullptr, nullptr, nullptr);
    g_object_unref(launcher);
}

void MainWindow::refreshHistory() {
    GtkWidget *row = gtk_widget_get_first_child(history_list_);
    while (row) {
        GtkWidget *next = gtk_widget_get_next_sibling(row);
        gtk_list_box_remove(GTK_LIST_BOX(history_list_), row);
        row = next;
    }

    for (const auto &entry : history_store_.list()) {
        GtkWidget *rowBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(rowBox, 4);
        gtk_widget_set_margin_end(rowBox, 4);
        gtk_widget_set_margin_top(rowBox, 4);
        gtk_widget_set_margin_bottom(rowBox, 4);

        GtkWidget *image = gtk_picture_new();
        GdkPixbuf *thumb = history_store_.loadThumbnail(entry.path, 96);
        if (thumb) {
            GdkTexture *tex = gdk_texture_new_for_pixbuf(thumb);
            gtk_picture_set_paintable(GTK_PICTURE(image), GDK_PAINTABLE(tex));
            g_object_unref(tex);
            g_object_unref(thumb);
        }
        gtk_widget_set_size_request(image, 90, 56);
        gtk_box_append(GTK_BOX(rowBox), image);

        g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local(entry.mtime);
        g_autofree gchar *timeLabel = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S");
        GtkWidget *nameLabel = gtk_label_new(timeLabel);
        gtk_label_set_xalign(GTK_LABEL(nameLabel), 0);
        gtk_widget_set_hexpand(nameLabel, TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(nameLabel), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(rowBox), nameLabel);

        GtkWidget *deleteBtn = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_add_css_class(deleteBtn, "flat");
        gtk_widget_set_tooltip_text(deleteBtn, "Delete");
        g_object_set_data_full(G_OBJECT(deleteBtn), "scrotocol-path", g_strdup(entry.path.c_str()),
                                g_free);
        g_signal_connect(deleteBtn, "clicked", G_CALLBACK(onDeleteHistoryClickedTrampoline), this);
        gtk_box_append(GTK_BOX(rowBox), deleteBtn);

        g_object_set_data_full(G_OBJECT(rowBox), "scrotocol-path", g_strdup(entry.path.c_str()),
                                g_free);

        // Right-click context menu
        GtkWidget *ctxMenu = gtk_popover_new();
        gtk_widget_set_parent(ctxMenu, rowBox);
        gtk_popover_set_has_arrow(GTK_POPOVER(ctxMenu), FALSE);
        GtkWidget *ctxBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget *deleteItem = gtk_button_new_with_label("Delete");
        gtk_widget_add_css_class(deleteItem, "flat");
        g_object_set_data_full(G_OBJECT(deleteItem), "scrotocol-path",
                                g_strdup(entry.path.c_str()), g_free);
        g_signal_connect(deleteItem, "clicked", G_CALLBACK(onDeleteHistoryClickedTrampoline), this);
        gtk_box_append(GTK_BOX(ctxBox), deleteItem);
        gtk_popover_set_child(GTK_POPOVER(ctxMenu), ctxBox);
        GtkGesture *rightClick = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rightClick), 3);
        g_signal_connect(rightClick, "pressed", G_CALLBACK(onRowRightClicked), ctxMenu);
        gtk_widget_add_controller(rowBox, GTK_EVENT_CONTROLLER(rightClick));

        // Middle-button drag: always provides the file URI
        auto *midCtx = new RowDragCtx{g_strdup(entry.path.c_str()), false};
        GtkDragSource *midDrag = gtk_drag_source_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(midDrag), 2);
        g_signal_connect_data(midDrag, "prepare", G_CALLBACK(onDragPrepare), midCtx,
                               dragCtxNotify, (GConnectFlags)0);
        gtk_widget_add_controller(rowBox, GTK_EVENT_CONTROLLER(midDrag));

        // Shift+primary drag: provides the file URI only when Shift is held
        auto *shiftCtx = new RowDragCtx{g_strdup(entry.path.c_str()), true};
        GtkDragSource *shiftDrag = gtk_drag_source_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(shiftDrag), 1);
        g_signal_connect_data(shiftDrag, "prepare", G_CALLBACK(onDragPrepare), shiftCtx,
                               dragCtxNotify, (GConnectFlags)0);
        gtk_widget_add_controller(rowBox, GTK_EVENT_CONTROLLER(shiftDrag));

        gtk_list_box_append(GTK_LIST_BOX(history_list_), rowBox);
    }
}

void MainWindow::loadFromHistory(const std::string &path) {
    GError *error = nullptr;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path.c_str(), &error);
    if (!pixbuf) {
        setStatus(std::string("Failed to load: ") + (error ? error->message : ""));
        g_clear_error(&error);
        return;
    }
    crop_overlay_.setImage(pixbuf);
    g_object_unref(pixbuf);
    setStatus("Loaded from history");
}

void MainWindow::deleteHistoryEntry(const std::string &path) {
    history_store_.remove(path);
    refreshHistory();
}

void MainWindow::updateTaskbarIcon(GdkPixbuf *pixbuf) {
    if (!Config::instance().taskbarIconUseCapture())
        return;

    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window_));
    if (!surface || !GDK_IS_X11_SURFACE(surface))
        return;

    static const int kSizes[] = {32, 48};
    std::vector<unsigned long> data;

    for (int sz : kSizes) {
        GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pixbuf, sz, sz, GDK_INTERP_BILINEAR);
        if (!scaled) continue;

        int w          = gdk_pixbuf_get_width(scaled);
        int h          = gdk_pixbuf_get_height(scaled);
        int rowstride  = gdk_pixbuf_get_rowstride(scaled);
        int n_channels = gdk_pixbuf_get_n_channels(scaled);
        bool has_alpha = gdk_pixbuf_get_has_alpha(scaled);
        guchar *pixels = gdk_pixbuf_get_pixels(scaled);

        data.push_back(static_cast<unsigned long>(w));
        data.push_back(static_cast<unsigned long>(h));

        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                guchar *p = pixels + y * rowstride + x * n_channels;
                unsigned long a = has_alpha ? p[3] : 255u;
                unsigned long r = p[0], g = p[1], b = p[2];
                data.push_back((a << 24) | (r << 16) | (g << 8) | b);
            }
        }
        g_object_unref(scaled);
    }

    Display *dpy  = gdk_x11_display_get_xdisplay(gdk_surface_get_display(surface));
    Window   xwin = gdk_x11_surface_get_xid(GDK_X11_SURFACE(surface));
    Atom     atom = XInternAtom(dpy, "_NET_WM_ICON", False);

    XChangeProperty(dpy, xwin, atom, XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<unsigned char *>(data.data()),
                    static_cast<int>(data.size()));
    XFlush(dpy);
}

void MainWindow::resetTaskbarIcon() {
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window_));
    if (!surface || !GDK_IS_X11_SURFACE(surface))
        return;

    Display *dpy  = gdk_x11_display_get_xdisplay(gdk_surface_get_display(surface));
    Window   xwin = gdk_x11_surface_get_xid(GDK_X11_SURFACE(surface));
    Atom     atom = XInternAtom(dpy, "_NET_WM_ICON", False);

    XDeleteProperty(dpy, xwin, atom);
    XFlush(dpy);

    gtk_window_set_icon_name(GTK_WINDOW(window_), "accessories-screenshot-tool");
}

void MainWindow::onCaptureButtonClickedTrampoline(GtkButton *button, gpointer userData) {
    auto *self = static_cast<MainWindow *>(userData);
    int modeInt = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "scrotocol-mode"));
    self->onCaptureButtonClicked(static_cast<CaptureMode>(modeInt));
}

void MainWindow::onApplyCropClickedTrampoline(GtkButton *, gpointer userData) {
    static_cast<MainWindow *>(userData)->onApplyCropClicked();
}

void MainWindow::onResetCropClickedTrampoline(GtkButton *, gpointer userData) {
    static_cast<MainWindow *>(userData)->onResetCropClicked();
}

void MainWindow::onCopyClickedTrampoline(GtkButton *, gpointer userData) {
    static_cast<MainWindow *>(userData)->onCopyClicked();
}

void MainWindow::onSaveAsClickedTrampoline(GtkButton *, gpointer userData) {
    static_cast<MainWindow *>(userData)->onSaveAsClicked();
}

void MainWindow::onMinimizeClickedTrampoline(GtkButton *, gpointer userData) {
    static_cast<MainWindow *>(userData)->onMinimizeClicked();
}

void MainWindow::onOpenFolderClickedTrampoline(GtkButton *, gpointer userData) {
    static_cast<MainWindow *>(userData)->onOpenFolderClicked();
}

void MainWindow::onDelayChangedTrampoline(GtkSpinButton *spin, gpointer userData) {
    Config::instance().setDefaultDelaySeconds(gtk_spin_button_get_value_as_int(spin));
}

void MainWindow::onDeleteHistoryClickedTrampoline(GtkButton *button, gpointer userData) {
    auto *self = static_cast<MainWindow *>(userData);
    const char *path =
        static_cast<const char *>(g_object_get_data(G_OBJECT(button), "scrotocol-path"));
    if (path)
        self->deleteHistoryEntry(path);
}

void MainWindow::onHistoryRowActivatedTrampoline(GtkListBox *, GtkListBoxRow *row,
                                                  gpointer userData) {
    auto *self = static_cast<MainWindow *>(userData);
    GtkWidget *child = gtk_list_box_row_get_child(row);
    const char *path =
        static_cast<const char *>(g_object_get_data(G_OBJECT(child), "scrotocol-path"));
    if (path)
        self->loadFromHistory(path);
}

void MainWindow::onMinimizeBeforeCaptureToggledTrampoline(GObject *obj, GParamSpec *, gpointer) {
    Config::instance().setMinimizeBeforeCapture(
        gtk_check_button_get_active(GTK_CHECK_BUTTON(obj)));
}

void MainWindow::onTaskbarIconToggledTrampoline(GObject *obj, GParamSpec *, gpointer userData) {
    auto *self = static_cast<MainWindow *>(userData);
    bool active = gtk_check_button_get_active(GTK_CHECK_BUTTON(obj));
    Config::instance().setTaskbarIconUseCapture(active);
    if (!active)
        self->resetTaskbarIcon();
}
