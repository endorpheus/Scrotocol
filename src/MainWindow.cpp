#include "MainWindow.h"

#include "Config.h"

#include <algorithm>

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

    GFile *histDir = g_file_new_for_path(Config::instance().historyDir().c_str());
    dir_monitor_ = g_file_monitor_directory(histDir, G_FILE_MONITOR_NONE, nullptr, nullptr);
    g_object_unref(histDir);
    if (dir_monitor_) {
        g_signal_connect(dir_monitor_, "changed",
                         G_CALLBACK(onHistoryDirChangedTrampoline), this);
    }
}

MainWindow::~MainWindow() {
    if (dir_monitor_) {
        g_file_monitor_cancel(dir_monitor_);
        g_object_unref(dir_monitor_);
    }
}

void MainWindow::present() { gtk_window_present(GTK_WINDOW(window_)); }

void MainWindow::setStatus(const std::string &text) {
    gtk_label_set_text(GTK_LABEL(status_label_), text.c_str());
}

void MainWindow::buildUi() {
    window_ = gtk_application_window_new(app_);
    gtk_window_set_title(GTK_WINDOW(window_), "Scrotocol");
    gtk_window_set_default_size(GTK_WINDOW(window_), 980, 640);
    gtk_window_set_icon_name(GTK_WINDOW(window_), "accessories-screenshot-tool");
    gtk_window_set_deletable(GTK_WINDOW(window_), FALSE);

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

    GtkWidget *scaleRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(scaleRow), gtk_label_new("Capture scale:"));
    capture_scale_spin_ = gtk_spin_button_new_with_range(1, 4, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(capture_scale_spin_),
                               Config::instance().captureScale());
    gtk_widget_set_tooltip_text(capture_scale_spin_,
        "Upscale captured image (bilinear) before editing.\n"
        "1 = native, 2–4 = upsampled for smoother cropping.");
    g_signal_connect(capture_scale_spin_, "value-changed",
                     G_CALLBACK(onCaptureScaleChangedTrampoline), nullptr);
    gtk_box_append(GTK_BOX(scaleRow), capture_scale_spin_);
    gtk_box_append(GTK_BOX(settingsBox), scaleRow);

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

    // --- Right: tabbed pane (History / Filters) ---
    GtkWidget *rightPane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(rightPane, 260, -1);
    gtk_widget_set_margin_end(rightPane, 6);
    gtk_widget_set_margin_top(rightPane, 6);
    gtk_widget_set_margin_bottom(rightPane, 6);

    GtkWidget *stack = gtk_stack_new();
    gtk_widget_set_vexpand(stack, TRUE);
    gtk_widget_set_hexpand(stack, TRUE);

    GtkWidget *switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(stack));
    gtk_widget_set_hexpand(switcher, TRUE);
    gtk_widget_set_halign(switcher, GTK_ALIGN_FILL);
    gtk_widget_set_margin_bottom(switcher, 4);

    gtk_box_append(GTK_BOX(rightPane), switcher);
    gtk_box_append(GTK_BOX(rightPane), stack);

    // History page
    GtkWidget *historyBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(historyBox, 4);
    gtk_widget_set_margin_end(historyBox, 4);
    gtk_widget_set_margin_top(historyBox, 4);
    gtk_widget_set_margin_bottom(historyBox, 4);

    GtkWidget *histScrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(histScrolled, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(histScrolled), GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    history_list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(history_list_), GTK_SELECTION_NONE);
    g_signal_connect(history_list_, "row-activated", G_CALLBACK(onHistoryRowActivatedTrampoline),
                      this);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(histScrolled), history_list_);
    gtk_widget_set_vexpand(historyBox, TRUE);
    gtk_box_append(GTK_BOX(historyBox), histScrolled);
    history_page_ = gtk_stack_add_titled(GTK_STACK(stack), historyBox, "history", "History");

    // Filters tab
    GtkWidget *filtersScrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(filtersScrolled), GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    GtkWidget *filtersBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(filtersBox, 8);
    gtk_widget_set_margin_end(filtersBox, 8);
    gtk_widget_set_margin_top(filtersBox, 6);
    gtk_widget_set_margin_bottom(filtersBox, 6);

    auto makeSection = [](const char *title, GtkWidget *box) {
        GtkWidget *lbl = gtk_label_new(title);
        gtk_widget_add_css_class(lbl, "heading");
        gtk_label_set_xalign(GTK_LABEL(lbl), 0);
        gtk_widget_set_margin_top(lbl, 6);
        gtk_box_append(GTK_BOX(box), lbl);
        gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    };

    auto makeScaleRow = [&](const char *label, double lo, double hi, double step,
                             double def, int filterTag) -> GtkWidget * {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *lbl = gtk_label_new(label);
        gtk_widget_set_size_request(lbl, 80, -1);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0);
        GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, lo, hi, step);
        gtk_range_set_value(GTK_RANGE(scale), def);
        gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
        gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
        gtk_widget_set_hexpand(scale, TRUE);
        g_object_set_data(G_OBJECT(scale), "fp", GINT_TO_POINTER(filterTag));
        g_signal_connect(scale, "value-changed", G_CALLBACK(onFilterScaleChangedTrampoline), this);
        gtk_box_append(GTK_BOX(row), lbl);
        gtk_box_append(GTK_BOX(row), scale);
        return scale;
    };

    // Adjust section
    makeSection("Adjust", filtersBox);
    filter_brightness_scale_ = makeScaleRow("Brightness", -1.0, 1.0, 0.01, 0.0, 0);
    gtk_box_append(GTK_BOX(filtersBox),
                   gtk_widget_get_parent(filter_brightness_scale_));
    filter_saturation_scale_ = makeScaleRow("Saturation", 0.0, 2.0, 0.01, 1.0, 1);
    gtk_box_append(GTK_BOX(filtersBox),
                   gtk_widget_get_parent(filter_saturation_scale_));
    filter_vignette_scale_ = makeScaleRow("Vignette", 0.0, 1.0, 0.01, 0.0, 2);
    gtk_box_append(GTK_BOX(filtersBox),
                   gtk_widget_get_parent(filter_vignette_scale_));

    GtkWidget *pixRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *pixLbl = gtk_label_new("Pixelate");
    gtk_widget_set_size_request(pixLbl, 80, -1);
    gtk_label_set_xalign(GTK_LABEL(pixLbl), 0);
    filter_pixelate_spin_ = gtk_spin_button_new_with_range(0, 64, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(filter_pixelate_spin_), 0);
    gtk_widget_set_tooltip_text(filter_pixelate_spin_, "0 = off, N = block size in pixels");
    g_signal_connect(filter_pixelate_spin_, "value-changed",
                     G_CALLBACK(onPixelateChangedTrampoline), this);
    gtk_box_append(GTK_BOX(pixRow), pixLbl);
    gtk_box_append(GTK_BOX(pixRow), filter_pixelate_spin_);
    gtk_box_append(GTK_BOX(filtersBox), pixRow);

    // Luma Key section
    makeSection("Luma Key", filtersBox);
    GtkWidget *lumaEnRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    filter_lumakey_check_ = gtk_check_button_new_with_label("Enable");
    filter_lumakey_invert_ = gtk_check_button_new_with_label("Invert");
    g_signal_connect(filter_lumakey_check_, "notify::active",
                     G_CALLBACK(onLumaKeyToggledTrampoline), this);
    g_signal_connect(filter_lumakey_invert_, "notify::active",
                     G_CALLBACK(onLumaKeyInvertToggledTrampoline), this);
    gtk_box_append(GTK_BOX(lumaEnRow), filter_lumakey_check_);
    gtk_box_append(GTK_BOX(lumaEnRow), filter_lumakey_invert_);
    gtk_box_append(GTK_BOX(filtersBox), lumaEnRow);
    filter_lumakey_scale_ = makeScaleRow("Threshold", 0.0, 1.0, 0.01, 0.5, 3);
    gtk_widget_set_sensitive(gtk_widget_get_parent(filter_lumakey_scale_), FALSE);
    gtk_box_append(GTK_BOX(filtersBox),
                   gtk_widget_get_parent(filter_lumakey_scale_));

    // Text Overlays section
    makeSection("Text Overlays", filtersBox);
    filter_text_entry_ = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(filter_text_entry_), "Text to add…");
    gtk_box_append(GTK_BOX(filtersBox), filter_text_entry_);

    GtkWidget *textOptsRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *szLbl = gtk_label_new("Size:");
    filter_text_size_spin_ = gtk_spin_button_new_with_range(8, 256, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(filter_text_size_spin_), 24);
    gtk_widget_set_size_request(filter_text_size_spin_, 60, -1);

    // Self-contained RGBA color picker (popover with sliders — no portal dependency)
    GtkWidget *colorPopover = gtk_popover_new();
    GtkWidget *colorBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(colorBox, 8);
    gtk_widget_set_margin_end(colorBox, 8);
    gtk_widget_set_margin_top(colorBox, 6);
    gtk_widget_set_margin_bottom(colorBox, 6);

    auto makeColorRow = [&](const char *lbl, GtkWidget **scale, double init) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *l = gtk_label_new(lbl);
        gtk_widget_set_size_request(l, 14, -1);
        *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.004);
        gtk_range_set_value(GTK_RANGE(*scale), init);
        gtk_scale_set_draw_value(GTK_SCALE(*scale), TRUE);
        gtk_scale_set_value_pos(GTK_SCALE(*scale), GTK_POS_RIGHT);
        gtk_widget_set_size_request(*scale, 130, -1);
        g_signal_connect(*scale, "value-changed",
                         G_CALLBACK(onTextColorScaleChangedTrampoline), this);
        gtk_box_append(GTK_BOX(row), l);
        gtk_box_append(GTK_BOX(row), *scale);
        gtk_box_append(GTK_BOX(colorBox), row);
    };
    makeColorRow("R", &filter_color_r_, 1.0);
    makeColorRow("G", &filter_color_g_, 1.0);
    makeColorRow("B", &filter_color_b_, 1.0);
    makeColorRow("A", &filter_color_a_, 1.0);

    filter_color_swatch_ = gtk_drawing_area_new();
    gtk_widget_set_size_request(filter_color_swatch_, -1, 22);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(filter_color_swatch_),
                                    drawColorSwatch, this, nullptr);
    gtk_box_append(GTK_BOX(colorBox), filter_color_swatch_);

    gtk_popover_set_child(GTK_POPOVER(colorPopover), colorBox);

    filter_text_color_btn_ = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(filter_text_color_btn_), "Color");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(filter_text_color_btn_), colorPopover);
    gtk_widget_set_tooltip_text(filter_text_color_btn_, "Text color (R/G/B/A)");

    gtk_box_append(GTK_BOX(textOptsRow), szLbl);
    gtk_box_append(GTK_BOX(textOptsRow), filter_text_size_spin_);
    gtk_box_append(GTK_BOX(textOptsRow), filter_text_color_btn_);
    gtk_box_append(GTK_BOX(filtersBox), textOptsRow);

    GtkWidget *addTextBtn = gtk_button_new_with_label("Add Text");
    g_signal_connect(addTextBtn, "clicked", G_CALLBACK(onAddTextClickedTrampoline), this);
    gtk_box_append(GTK_BOX(filtersBox), addTextBtn);

    GtkWidget *textScrolled = gtk_scrolled_window_new();
    gtk_widget_set_size_request(textScrolled, -1, 80);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(textScrolled), GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    filter_text_list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(filter_text_list_), GTK_SELECTION_SINGLE);
    g_signal_connect(filter_text_list_, "row-activated",
                     G_CALLBACK(onTextRowActivatedTrampoline), this);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(textScrolled), filter_text_list_);
    gtk_box_append(GTK_BOX(filtersBox), textScrolled);

    GtkWidget *resetBtn = gtk_button_new_with_label("Reset All Filters");
    gtk_widget_add_css_class(resetBtn, "destructive-action");
    g_signal_connect(resetBtn, "clicked", G_CALLBACK(onResetFiltersClickedTrampoline), this);
    gtk_widget_set_margin_top(resetBtn, 8);
    gtk_box_append(GTK_BOX(filtersBox), resetBtn);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(filtersScrolled), filtersBox);
    gtk_stack_add_titled(GTK_STACK(stack), filtersScrolled, "filters", "Filters");

    gtk_paned_set_end_child(GTK_PANED(paned), rightPane);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);

    gtk_window_set_child(GTK_WINDOW(window_), paned);

    crop_overlay_.setFilterEngine(&engine_);

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

    int cs = Config::instance().captureScale();
    if (cs > 1) {
        int sw = gdk_pixbuf_get_width(pixbuf)  * cs;
        int sh = gdk_pixbuf_get_height(pixbuf) * cs;
        GdkPixbuf *up = gdk_pixbuf_scale_simple(pixbuf, sw, sh, GDK_INTERP_BILINEAR);
        g_object_unref(pixbuf);
        pixbuf = up ? up : pixbuf;
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
    GdkPixbuf *img = crop_overlay_.renderFinal();
    if (!img) {
        setStatus("Nothing to copy yet");
        return;
    }
    GdkTexture *texture = gdk_texture_new_for_pixbuf(img);
    GdkClipboard *clipboard = gdk_display_get_clipboard(gtk_widget_get_display(window_));
    gdk_clipboard_set_texture(clipboard, texture);
    g_object_unref(texture);
    g_object_unref(img);
    setStatus("Copied to clipboard");
}

void MainWindow::onSaveAsClicked() {
    GdkPixbuf *img = crop_overlay_.renderFinal();
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

    auto *ctx = new SaveCtx{this, img};  // img already has a ref from renderFinal()
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
        if (gdk_pixbuf_save(ctx->pixbuf, path, "png", &saveError, nullptr)) {
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

    size_t count = history_store_.list().size();
    g_autofree gchar *title = g_strdup_printf("History (%zu)", count);
    gtk_stack_page_set_title(history_page_, title);
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

// --- filter panel handlers ---

void MainWindow::onFilterScaleChanged(GtkRange *range) {
    int tag = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(range), "fp"));
    double v = gtk_range_get_value(range);
    switch (tag) {
        case 0: engine_.brightness = static_cast<float>(v); break;
        case 1: engine_.saturation = static_cast<float>(v); break;
        case 2: engine_.vignette   = static_cast<float>(v); break;
        case 3: engine_.lumaKey    = static_cast<float>(v); break;
    }
    crop_overlay_.reapplyFilter();
}

void MainWindow::onPixelateChanged(GtkSpinButton *spin) {
    engine_.pixelate = gtk_spin_button_get_value_as_int(spin);
    crop_overlay_.reapplyFilter();
}

void MainWindow::onLumaKeyToggled(bool active) {
    engine_.lumaKeyEnabled = active;
    gtk_widget_set_sensitive(gtk_widget_get_parent(filter_lumakey_scale_), active);
    crop_overlay_.reapplyFilter();
}

void MainWindow::onLumaKeyInvertToggled(bool active) {
    engine_.lumaInvert = active;
    if (engine_.lumaKeyEnabled)
        crop_overlay_.reapplyFilter();
}

void MainWindow::onAddTextClicked() {
    const char *txt = gtk_editable_get_text(GTK_EDITABLE(filter_text_entry_));
    if (!txt || txt[0] == '\0') return;

    TextOverlay t;
    t.text     = txt;
    t.fontSize = static_cast<float>(gtk_spin_button_get_value(GTK_SPIN_BUTTON(filter_text_size_spin_)));
    t.r = static_cast<float>(gtk_range_get_value(GTK_RANGE(filter_color_r_)));
    t.g = static_cast<float>(gtk_range_get_value(GTK_RANGE(filter_color_g_)));
    t.b = static_cast<float>(gtk_range_get_value(GTK_RANGE(filter_color_b_)));
    t.a = static_cast<float>(gtk_range_get_value(GTK_RANGE(filter_color_a_)));
    engine_.texts.push_back(std::move(t));

    int newIdx = static_cast<int>(engine_.texts.size()) - 1;
    refreshTextList();
    crop_overlay_.setSelectedTextIdx(newIdx);
    gtk_editable_set_text(GTK_EDITABLE(filter_text_entry_), "");
}

void MainWindow::onTextColorScaleChanged() {
    int idx = crop_overlay_.selectedTextIdx();
    gtk_widget_queue_draw(filter_color_swatch_);
    if (idx < 0 || idx >= static_cast<int>(engine_.texts.size())) return;
    auto &t = engine_.texts[idx];
    t.r = static_cast<float>(gtk_range_get_value(GTK_RANGE(filter_color_r_)));
    t.g = static_cast<float>(gtk_range_get_value(GTK_RANGE(filter_color_g_)));
    t.b = static_cast<float>(gtk_range_get_value(GTK_RANGE(filter_color_b_)));
    t.a = static_cast<float>(gtk_range_get_value(GTK_RANGE(filter_color_a_)));
    crop_overlay_.setSelectedTextIdx(idx);
}

void MainWindow::onDeleteTextClicked(int idx) {
    if (idx < 0 || idx >= static_cast<int>(engine_.texts.size())) return;
    engine_.texts.erase(engine_.texts.begin() + idx);
    int sel = crop_overlay_.selectedTextIdx();
    if (sel == idx)
        crop_overlay_.setSelectedTextIdx(-1);
    else if (sel > idx)
        crop_overlay_.setSelectedTextIdx(sel - 1);
    refreshTextList();
}

void MainWindow::onTextRowActivated(int idx) {
    crop_overlay_.setSelectedTextIdx(idx);
    if (idx >= 0 && idx < static_cast<int>(engine_.texts.size())) {
        const auto &t = engine_.texts[idx];
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(filter_text_size_spin_), t.fontSize);
        gtk_range_set_value(GTK_RANGE(filter_color_r_), t.r);
        gtk_range_set_value(GTK_RANGE(filter_color_g_), t.g);
        gtk_range_set_value(GTK_RANGE(filter_color_b_), t.b);
        gtk_range_set_value(GTK_RANGE(filter_color_a_), t.a);
        gtk_widget_queue_draw(filter_color_swatch_);
    }
}

void MainWindow::onToggleTextVisibility(int idx, bool visible) {
    if (idx < 0 || idx >= static_cast<int>(engine_.texts.size())) return;
    engine_.texts[idx].visible = visible;
    crop_overlay_.setSelectedTextIdx(crop_overlay_.selectedTextIdx());
}

void MainWindow::onTextLayerDrop(int srcIdx, int dstIdx) {
    int n = static_cast<int>(engine_.texts.size());
    if (srcIdx == dstIdx || srcIdx < 0 || dstIdx < 0 || srcIdx >= n || dstIdx >= n) return;

    int sel = crop_overlay_.selectedTextIdx();

    if (srcIdx < dstIdx) {
        std::rotate(engine_.texts.begin() + srcIdx,
                    engine_.texts.begin() + srcIdx + 1,
                    engine_.texts.begin() + dstIdx + 1);
        if (sel == srcIdx)               sel = dstIdx;
        else if (sel > srcIdx && sel <= dstIdx) sel--;
    } else {
        std::rotate(engine_.texts.begin() + dstIdx,
                    engine_.texts.begin() + srcIdx,
                    engine_.texts.begin() + srcIdx + 1);
        if (sel == srcIdx)                sel = dstIdx;
        else if (sel >= dstIdx && sel < srcIdx) sel++;
    }

    refreshTextList();
    crop_overlay_.setSelectedTextIdx(sel);
}

void MainWindow::onResetFiltersClicked() {
    engine_.reset();
    gtk_range_set_value(GTK_RANGE(filter_brightness_scale_), 0.0);
    gtk_range_set_value(GTK_RANGE(filter_saturation_scale_), 1.0);
    gtk_range_set_value(GTK_RANGE(filter_vignette_scale_), 0.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(filter_pixelate_spin_), 0);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(filter_lumakey_check_), FALSE);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(filter_lumakey_invert_), FALSE);
    gtk_range_set_value(GTK_RANGE(filter_lumakey_scale_), 0.5);
    refreshTextList();
    crop_overlay_.setSelectedTextIdx(-1);
    crop_overlay_.reapplyFilter();
}

void MainWindow::refreshTextList() {
    GtkWidget *child = gtk_widget_get_first_child(filter_text_list_);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(filter_text_list_), child);
        child = next;
    }
    for (int i = 0; i < static_cast<int>(engine_.texts.size()); i++) {
        const auto &t = engine_.texts[i];

        GtkWidget *rowBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_set_margin_start(rowBox, 2);
        gtk_widget_set_margin_end(rowBox, 2);
        gtk_widget_set_margin_top(rowBox, 1);
        gtk_widget_set_margin_bottom(rowBox, 1);

        // Visibility toggle (eye)
        GtkWidget *eyeBtn = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(eyeBtn),
            t.visible ? "view-reveal-symbolic" : "view-conceal-symbolic");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(eyeBtn), t.visible);
        gtk_widget_add_css_class(eyeBtn, "flat");
        gtk_widget_set_tooltip_text(eyeBtn, "Toggle visibility");
        g_object_set_data(G_OBJECT(eyeBtn), "text-idx", GINT_TO_POINTER(i));
        g_signal_connect(eyeBtn, "notify::active",
                         G_CALLBACK(onToggleTextVisibilityTrampoline), this);

        GtkWidget *lbl = gtk_label_new(t.text.c_str());
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0);
        gtk_widget_set_hexpand(lbl, TRUE);

        GtkWidget *delBtn = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_add_css_class(delBtn, "flat");
        gtk_widget_set_tooltip_text(delBtn, "Remove");
        g_object_set_data(G_OBJECT(delBtn), "text-idx", GINT_TO_POINTER(i));
        g_signal_connect(delBtn, "clicked", G_CALLBACK(onDeleteTextClickedTrampoline), this);

        gtk_box_append(GTK_BOX(rowBox), eyeBtn);
        gtk_box_append(GTK_BOX(rowBox), lbl);
        gtk_box_append(GTK_BOX(rowBox), delBtn);

        // Drag source — carries source row index as G_TYPE_INT
        GtkDragSource *dragSrc = gtk_drag_source_new();
        gtk_drag_source_set_actions(dragSrc, GDK_ACTION_MOVE);
        g_object_set_data(G_OBJECT(dragSrc), "src-idx", GINT_TO_POINTER(i));
        g_signal_connect(dragSrc, "prepare",
                         G_CALLBACK(onTextLayerDragPrepareTrampoline), nullptr);
        gtk_widget_add_controller(rowBox, GTK_EVENT_CONTROLLER(dragSrc));

        // Drop target — accepts G_TYPE_INT (source row index)
        GtkDropTarget *dropTgt = gtk_drop_target_new(G_TYPE_INT, GDK_ACTION_MOVE);
        g_object_set_data(G_OBJECT(dropTgt), "dst-idx", GINT_TO_POINTER(i));
        g_signal_connect(dropTgt, "drop",
                         G_CALLBACK(onTextLayerDropTrampoline), this);
        gtk_widget_add_controller(rowBox, GTK_EVENT_CONTROLLER(dropTgt));

        gtk_list_box_append(GTK_LIST_BOX(filter_text_list_), rowBox);
    }
}

// --- filter trampolines ---

void MainWindow::onFilterScaleChangedTrampoline(GtkRange *range, gpointer userData) {
    static_cast<MainWindow *>(userData)->onFilterScaleChanged(range);
}

void MainWindow::onPixelateChangedTrampoline(GtkSpinButton *spin, gpointer userData) {
    static_cast<MainWindow *>(userData)->onPixelateChanged(spin);
}

void MainWindow::onLumaKeyToggledTrampoline(GObject *obj, GParamSpec *, gpointer userData) {
    auto *self = static_cast<MainWindow *>(userData);
    self->onLumaKeyToggled(gtk_check_button_get_active(GTK_CHECK_BUTTON(obj)));
}

void MainWindow::onLumaKeyInvertToggledTrampoline(GObject *obj, GParamSpec *, gpointer userData) {
    auto *self = static_cast<MainWindow *>(userData);
    self->onLumaKeyInvertToggled(gtk_check_button_get_active(GTK_CHECK_BUTTON(obj)));
}

void MainWindow::onAddTextClickedTrampoline(GtkButton *, gpointer userData) {
    static_cast<MainWindow *>(userData)->onAddTextClicked();
}

void MainWindow::onTextColorScaleChangedTrampoline(GtkRange *, gpointer userData) {
    static_cast<MainWindow *>(userData)->onTextColorScaleChanged();
}

void MainWindow::drawColorSwatch(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer userData) {
    auto *self = static_cast<MainWindow *>(userData);
    // checkerboard background to show alpha
    const int tile = 6;
    for (int ty = 0; ty < h; ty += tile) {
        for (int tx = 0; tx < w; tx += tile) {
            bool light = ((tx / tile + ty / tile) & 1) == 0;
            cairo_set_source_rgb(cr, light ? 0.75 : 0.5, light ? 0.75 : 0.5, light ? 0.75 : 0.5);
            cairo_rectangle(cr, tx, ty, tile, tile);
            cairo_fill(cr);
        }
    }
    double r = gtk_range_get_value(GTK_RANGE(self->filter_color_r_));
    double g = gtk_range_get_value(GTK_RANGE(self->filter_color_g_));
    double b = gtk_range_get_value(GTK_RANGE(self->filter_color_b_));
    double a = gtk_range_get_value(GTK_RANGE(self->filter_color_a_));
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);
}

void MainWindow::onDeleteTextClickedTrampoline(GtkButton *btn, gpointer userData) {
    auto *self = static_cast<MainWindow *>(userData);
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "text-idx"));
    self->onDeleteTextClicked(idx);
}

void MainWindow::onTextRowActivatedTrampoline(GtkListBox *, GtkListBoxRow *row, gpointer userData) {
    auto *self = static_cast<MainWindow *>(userData);
    self->onTextRowActivated(gtk_list_box_row_get_index(row));
}

void MainWindow::onResetFiltersClickedTrampoline(GtkButton *, gpointer userData) {
    static_cast<MainWindow *>(userData)->onResetFiltersClicked();
}

void MainWindow::onToggleTextVisibilityTrampoline(GObject *obj, GParamSpec *, gpointer userData) {
    auto *self = static_cast<MainWindow *>(userData);
    int idx     = GPOINTER_TO_INT(g_object_get_data(obj, "text-idx"));
    bool active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(obj));
    gtk_button_set_icon_name(GTK_BUTTON(obj),
        active ? "view-reveal-symbolic" : "view-conceal-symbolic");
    self->onToggleTextVisibility(idx, active);
}

GdkContentProvider *MainWindow::onTextLayerDragPrepareTrampoline(GtkDragSource *src,
                                                                   double, double, gpointer) {
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(src), "src-idx"));
    GValue val = G_VALUE_INIT;
    g_value_init(&val, G_TYPE_INT);
    g_value_set_int(&val, idx);
    GdkContentProvider *prov = gdk_content_provider_new_for_value(&val);
    g_value_unset(&val);
    return prov;
}

gboolean MainWindow::onTextLayerDropTrampoline(GtkDropTarget *target, const GValue *value,
                                                double, double, gpointer userData) {
    auto *self = static_cast<MainWindow *>(userData);
    int srcIdx  = g_value_get_int(value);
    int dstIdx  = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(target), "dst-idx"));
    self->onTextLayerDrop(srcIdx, dstIdx);
    return TRUE;
}

void MainWindow::onCaptureScaleChangedTrampoline(GtkSpinButton *spin, gpointer) {
    Config::instance().setCaptureScale(gtk_spin_button_get_value_as_int(spin));
}

void MainWindow::onHistoryDirChangedTrampoline(GFileMonitor *, GFile *, GFile *,
                                               GFileMonitorEvent eventType, gpointer userData) {
    switch (eventType) {
        case G_FILE_MONITOR_EVENT_CREATED:
        case G_FILE_MONITOR_EVENT_DELETED:
        case G_FILE_MONITOR_EVENT_MOVED_IN:
        case G_FILE_MONITOR_EVENT_MOVED_OUT:
        case G_FILE_MONITOR_EVENT_RENAMED:
            static_cast<MainWindow *>(userData)->refreshHistory();
            break;
        default:
            break;
    }
}
