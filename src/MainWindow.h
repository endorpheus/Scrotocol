#pragma once

#include "Capture.h"
#include "CropOverlay.h"
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
    GtkWidget *window_ = nullptr;
    GtkWidget *status_label_ = nullptr;
    GtkWidget *delay_spin_ = nullptr;
    GtkWidget *history_list_ = nullptr;

    CropOverlay crop_overlay_;
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
};
