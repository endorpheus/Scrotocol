#pragma once

#include "FilterEngine.h"
#include <gtk/gtk.h>
#include <vector>

// GtkOverlay showing an image with:
//   - contain-fit display (GtkPicture)
//   - draggable crop-selection rectangle
//   - live text overlay rendering (via FilterEngine)
//
// Non-destructive filter pipeline:
//   original_ → FilterEngine::applyPixelFilters → filtered_ → (crop) → pixbuf_
//
// Text overlays are drawn by drawFunc on top of pixbuf_; they are baked in
// only when renderFinal() is called (for copy / save).
class CropOverlay {
public:
    CropOverlay();
    ~CropOverlay();

    GtkWidget *widget() const { return overlay_; }

    // Replaces the source image. Clears crop and selection. Reapplies current engine.
    void setImage(GdkPixbuf *pixbuf);

    // Attach a FilterEngine whose pixel filters drive the filtered_ pixbuf.
    // Does not take ownership.
    void setFilterEngine(FilterEngine *engine);

    // Rebuilds filtered_ from original_ and re-applies current crop rect.
    // Call this after any pixel-filter parameter changes.
    void reapplyFilter();

    // Current working pixbuf (pixel-filtered + cropped, no text baked). May be nullptr.
    GdkPixbuf *image() const { return pixbuf_; }

    // Returns a new GdkPixbuf with text overlays baked in. Caller owns the ref.
    // Falls back to a ref to pixbuf_ if there are no texts.
    GdkPixbuf *renderFinal() const;

    bool hasSelection() const { return has_selection_; }

    void applyCrop();
    void resetCrop();

    int  selectedTextIdx() const { return selectedTextIdx_; }
    void setSelectedTextIdx(int idx);

private:
    GtkWidget *overlay_;
    GtkWidget *picture_;
    GtkWidget *draw_area_;
    GtkGesture *drag_gesture_;

    GdkPixbuf *original_ = nullptr;  // raw source, never modified
    GdkPixbuf *filtered_ = nullptr;  // pixel-filtered version of original_
    GdkPixbuf *pixbuf_   = nullptr;  // cropped region of filtered_

    FilterEngine *engine_ = nullptr;

    bool hasCrop_ = false;
    int  cropX_ = 0, cropY_ = 0, cropW_ = 0, cropH_ = 0;

    bool   dragging_     = false;
    bool   has_selection_ = false;
    double drag_start_x_ = 0, drag_start_y_ = 0;
    double sel_x0_ = 0, sel_y0_ = 0, sel_x1_ = 0, sel_y1_ = 0;

    enum class DragMode { None, CropDraw, TextMove };
    DragMode dragMode_     = DragMode::None;
    int      dragTextIdx_  = -1;
    int selectedTextIdx_   = -1;
    float dragTextStartNX_ = 0, dragTextStartNY_ = 0;
    float dragTextStartPx_ = 0, dragTextStartPy_ = 0; // widget coords at drag start

    struct TextBounds { float x, y, w, h; }; // widget coords, updated each draw
    std::vector<TextBounds> textBoundsCache_;

    void refreshPicture();
    void computeContainLayout(int widgetW, int widgetH,
                               double &x, double &y, double &w, double &h) const;
    void widgetToImage(double wx, double wy, double &ix, double &iy) const;
    int  hitTestText(double wx, double wy) const;

    static void drawFunc(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                          gpointer userData);
    static void onDragBegin (GtkGestureDrag *gesture, double x, double y, gpointer userData);
    static void onDragUpdate(GtkGestureDrag *gesture, double x, double y, gpointer userData);
    static void onDragEnd   (GtkGestureDrag *gesture, double x, double y, gpointer userData);
};
