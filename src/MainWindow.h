#pragma once

#include "Capture.h"
#include "CropOverlay.h"
#include "FilterEngine.h"
#include "HistoryStore.h"
#include "TrayIcon.h"

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>

class MainWindow {
public:
    explicit MainWindow(GtkApplication *app);
    ~MainWindow();

    void present();
    void setStatus(const std::string &text);
    void doCapture(CaptureMode mode);

private:
    GtkApplication *app_;
    GtkWidget *window_       = nullptr;
    GtkWidget *status_label_ = nullptr;
    GtkWidget *delay_spin_   = nullptr;
    GtkWidget    *history_list_  = nullptr;
    GtkStackPage *history_page_ = nullptr;
    GFileMonitor *dir_monitor_  = nullptr;
    GtkWidget *capture_scale_spin_ = nullptr;

    // Filter tab widgets
    GtkWidget *filter_brightness_scale_   = nullptr;
    GtkWidget *filter_saturation_scale_   = nullptr;
    GtkWidget *filter_vignette_scale_     = nullptr;
    GtkWidget *filter_pixelate_spin_      = nullptr;
    GtkWidget *filter_lumakey_check_      = nullptr;
    GtkWidget *filter_lumakey_scale_      = nullptr;
    GtkWidget *filter_lumakey_invert_     = nullptr;
    GtkWidget *filter_text_entry_         = nullptr;
    GtkWidget *filter_text_size_spin_     = nullptr;
    GtkWidget *filter_text_color_btn_  = nullptr;  // GtkMenuButton
    GtkWidget *filter_color_r_         = nullptr;
    GtkWidget *filter_color_g_         = nullptr;
    GtkWidget *filter_color_b_         = nullptr;
    GtkWidget *filter_color_a_         = nullptr;
    GtkWidget *filter_color_swatch_    = nullptr;
    GtkWidget *filter_text_list_          = nullptr;

    FilterEngine engine_;
    CropOverlay  crop_overlay_;
    HistoryStore history_store_;
    std::unique_ptr<TrayIcon> tray_;

    void buildUi();

    void onCaptureButtonClicked(CaptureMode mode);
    void runCountdown(int seconds, std::function<void()> done);
    void beginCapture(CaptureMode mode);
    void onCaptureFinished(bool success, const std::string &path, const std::string &error);

    void onApplyCropClicked();
    void onResetCropClicked();
    void onCopyClicked();
    void onSaveAsClicked();
    void onMinimizeClicked();
    void onOpenFolderClicked();
    void toggleVisibility();

    void refreshHistory();
    void loadFromHistory(const std::string &path);
    void deleteHistoryEntry(const std::string &path);

    void updateTaskbarIcon(GdkPixbuf *pixbuf);
    void resetTaskbarIcon();

    // Filter panel
    void refreshTextList();
    void onFilterScaleChanged(GtkRange *range);
    void onPixelateChanged(GtkSpinButton *spin);
    void onLumaKeyToggled(bool active);
    void onLumaKeyInvertToggled(bool active);
    void onAddTextClicked();
    void onDeleteTextClicked(int idx);
    void onTextRowActivated(int idx);
    void onTextColorScaleChanged();
    void onToggleTextVisibility(int idx, bool visible);
    void onTextLayerDrop(int srcIdx, int dstIdx);
    void onResetFiltersClicked();

    // --- trampolines ---
    static void onCaptureButtonClickedTrampoline(GtkButton *button, gpointer userData);
    static void onApplyCropClickedTrampoline(GtkButton *button, gpointer userData);
    static void onResetCropClickedTrampoline(GtkButton *button, gpointer userData);
    static void onCopyClickedTrampoline(GtkButton *button, gpointer userData);
    static void onSaveAsClickedTrampoline(GtkButton *button, gpointer userData);
    static void onMinimizeClickedTrampoline(GtkButton *button, gpointer userData);
    static void onOpenFolderClickedTrampoline(GtkButton *button, gpointer userData);
    static void onDelayChangedTrampoline(GtkSpinButton *spin, gpointer userData);
    static void onDeleteHistoryClickedTrampoline(GtkButton *button, gpointer userData);
    static void onHistoryRowActivatedTrampoline(GtkListBox *box, GtkListBoxRow *row,
                                                 gpointer userData);
    static void onSaveDialogFinished(GObject *source, GAsyncResult *result, gpointer userData);
    static void onMinimizeBeforeCaptureToggledTrampoline(GObject *obj, GParamSpec *pspec,
                                                          gpointer userData);
    static void onTaskbarIconToggledTrampoline(GObject *obj, GParamSpec *pspec, gpointer userData);
    static void onCaptureScaleChangedTrampoline(GtkSpinButton *spin, gpointer userData);

    static void onFilterScaleChangedTrampoline(GtkRange *range, gpointer userData);
    static void onPixelateChangedTrampoline(GtkSpinButton *spin, gpointer userData);
    static void onLumaKeyToggledTrampoline(GObject *obj, GParamSpec *pspec, gpointer userData);
    static void onLumaKeyInvertToggledTrampoline(GObject *obj, GParamSpec *pspec, gpointer userData);
    static void onAddTextClickedTrampoline(GtkButton *btn, gpointer userData);
    static void onTextColorScaleChangedTrampoline(GtkRange *range, gpointer userData);
    static void drawColorSwatch(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer userData);
    static void onDeleteTextClickedTrampoline(GtkButton *btn, gpointer userData);
    static void onTextRowActivatedTrampoline(GtkListBox *box, GtkListBoxRow *row, gpointer userData);
    static void onToggleTextVisibilityTrampoline(GObject *obj, GParamSpec *pspec, gpointer userData);
    static GdkContentProvider *onTextLayerDragPrepareTrampoline(GtkDragSource *src, double x,
                                                                  double y, gpointer userData);
    static gboolean onTextLayerDropTrampoline(GtkDropTarget *target, const GValue *value,
                                               double x, double y, gpointer userData);
    static void onResetFiltersClickedTrampoline(GtkButton *btn, gpointer userData);
    static void onHistoryDirChangedTrampoline(GFileMonitor *monitor, GFile *file,
                                              GFile *otherFile, GFileMonitorEvent eventType,
                                              gpointer userData);
};
