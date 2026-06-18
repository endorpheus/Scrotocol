#pragma once

#include <gtk/gtk.h>

// A GtkOverlay showing an image (content-fit "contain") with a draggable
// crop-selection rectangle drawn on top. Coordinates are tracked in image
// pixel space so applyCrop() can subpixbuf directly.
class CropOverlay {
public:
    CropOverlay();
    ~CropOverlay();

    GtkWidget *widget() const { return overlay_; }

    // Replaces the displayed/working image. Takes a ref; clears any pending
    // selection and forgets the previous crop history.
    void setImage(GdkPixbuf *pixbuf);

    // Current working image (post-crop if a crop was applied), or nullptr.
    GdkPixbuf *image() const { return pixbuf_; }

    bool hasSelection() const { return has_selection_; }

    // Crops pixbuf_ to the current selection, replacing the working image.
    // No-op if there is no active selection.
    void applyCrop();

    // Restores the working image to the original (pre-crop) capture.
    void resetCrop();

private:
    GtkWidget *overlay_;
    GtkWidget *picture_;
    GtkWidget *draw_area_;
    GtkGesture *drag_gesture_;

    GdkPixbuf *original_ = nullptr;
    GdkPixbuf *pixbuf_ = nullptr;

    bool dragging_ = false;
    bool has_selection_ = false;
    double drag_start_x_ = 0, drag_start_y_ = 0;
    double sel_x0_ = 0, sel_y0_ = 0, sel_x1_ = 0, sel_y1_ = 0; // image pixel coords

    void refreshPicture();
    void computeContainLayout(int widgetW, int widgetH, double &x, double &y, double &w,
                               double &h) const;
    void widgetToImage(double wx, double wy, double &ix, double &iy) const;

    static void drawFunc(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                          gpointer userData);
    static void onDragBegin(GtkGestureDrag *gesture, double x, double y, gpointer userData);
    static void onDragUpdate(GtkGestureDrag *gesture, double x, double y, gpointer userData);
    static void onDragEnd(GtkGestureDrag *gesture, double x, double y, gpointer userData);
};
