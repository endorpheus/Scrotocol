#include "CropOverlay.h"

#include <algorithm>
#include <cmath>

CropOverlay::CropOverlay() {
    picture_ = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(picture_), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_hexpand(picture_, TRUE);
    gtk_widget_set_vexpand(picture_, TRUE);

    draw_area_ = gtk_drawing_area_new();
    gtk_widget_set_hexpand(draw_area_, TRUE);
    gtk_widget_set_vexpand(draw_area_, TRUE);
    gtk_widget_set_halign(draw_area_, GTK_ALIGN_FILL);
    gtk_widget_set_valign(draw_area_, GTK_ALIGN_FILL);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(draw_area_), drawFunc, this, nullptr);

    overlay_ = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay_), picture_);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay_), draw_area_);

    drag_gesture_ = gtk_gesture_drag_new();
    g_signal_connect(drag_gesture_, "drag-begin", G_CALLBACK(onDragBegin), this);
    g_signal_connect(drag_gesture_, "drag-update", G_CALLBACK(onDragUpdate), this);
    g_signal_connect(drag_gesture_, "drag-end", G_CALLBACK(onDragEnd), this);
    gtk_widget_add_controller(draw_area_, GTK_EVENT_CONTROLLER(drag_gesture_));
}

CropOverlay::~CropOverlay() {
    if (pixbuf_)
        g_object_unref(pixbuf_);
    if (original_)
        g_object_unref(original_);
}

void CropOverlay::setImage(GdkPixbuf *pixbuf) {
    if (pixbuf_)
        g_object_unref(pixbuf_);
    if (original_)
        g_object_unref(original_);

    original_ = pixbuf ? GDK_PIXBUF(g_object_ref(pixbuf)) : nullptr;
    pixbuf_ = pixbuf ? GDK_PIXBUF(g_object_ref(pixbuf)) : nullptr;
    has_selection_ = false;
    dragging_ = false;
    refreshPicture();
}

void CropOverlay::refreshPicture() {
    if (pixbuf_) {
        GdkTexture *texture = gdk_texture_new_for_pixbuf(pixbuf_);
        gtk_picture_set_paintable(GTK_PICTURE(picture_), GDK_PAINTABLE(texture));
        g_object_unref(texture);
    } else {
        gtk_picture_set_paintable(GTK_PICTURE(picture_), nullptr);
    }
    gtk_widget_queue_draw(draw_area_);
}

void CropOverlay::computeContainLayout(int widgetW, int widgetH, double &x, double &y, double &w,
                                        double &h) const {
    x = y = w = h = 0;
    if (!pixbuf_ || widgetW <= 0 || widgetH <= 0)
        return;

    double imgW = gdk_pixbuf_get_width(pixbuf_);
    double imgH = gdk_pixbuf_get_height(pixbuf_);
    if (imgW <= 0 || imgH <= 0)
        return;

    double scale = std::min(widgetW / imgW, widgetH / imgH);
    w = imgW * scale;
    h = imgH * scale;
    x = (widgetW - w) / 2.0;
    y = (widgetH - h) / 2.0;
}

void CropOverlay::widgetToImage(double wx, double wy, double &ix, double &iy) const {
    int widgetW = gtk_widget_get_width(draw_area_);
    int widgetH = gtk_widget_get_height(draw_area_);
    double x, y, w, h;
    computeContainLayout(widgetW, widgetH, x, y, w, h);

    if (w <= 0 || h <= 0 || !pixbuf_) {
        ix = iy = 0;
        return;
    }

    double imgW = gdk_pixbuf_get_width(pixbuf_);
    double imgH = gdk_pixbuf_get_height(pixbuf_);
    double scale = w / imgW;

    ix = std::clamp((wx - x) / scale, 0.0, imgW);
    iy = std::clamp((wy - y) / scale, 0.0, imgH);
}

void CropOverlay::onDragBegin(GtkGestureDrag *, double x, double y, gpointer userData) {
    auto *self = static_cast<CropOverlay *>(userData);
    if (!self->pixbuf_)
        return;
    self->dragging_ = true;
    self->widgetToImage(x, y, self->sel_x0_, self->sel_y0_);
    self->sel_x1_ = self->sel_x0_;
    self->sel_y1_ = self->sel_y0_;
    self->has_selection_ = true;
    gtk_widget_queue_draw(self->draw_area_);
}

void CropOverlay::onDragUpdate(GtkGestureDrag *gesture, double, double, gpointer userData) {
    auto *self = static_cast<CropOverlay *>(userData);
    if (!self->dragging_)
        return;

    double startX, startY, offsetX, offsetY;
    gtk_gesture_drag_get_start_point(gesture, &startX, &startY);
    gtk_gesture_drag_get_offset(gesture, &offsetX, &offsetY);
    self->widgetToImage(startX + offsetX, startY + offsetY, self->sel_x1_, self->sel_y1_);
    gtk_widget_queue_draw(self->draw_area_);
}

void CropOverlay::onDragEnd(GtkGestureDrag *gesture, double, double, gpointer userData) {
    auto *self = static_cast<CropOverlay *>(userData);
    if (!self->dragging_)
        return;
    self->dragging_ = false;

    double startX, startY, offsetX, offsetY;
    gtk_gesture_drag_get_start_point(gesture, &startX, &startY);
    gtk_gesture_drag_get_offset(gesture, &offsetX, &offsetY);
    self->widgetToImage(startX + offsetX, startY + offsetY, self->sel_x1_, self->sel_y1_);

    if (std::abs(self->sel_x1_ - self->sel_x0_) < 2 || std::abs(self->sel_y1_ - self->sel_y0_) < 2)
        self->has_selection_ = false;

    gtk_widget_queue_draw(self->draw_area_);
}

void CropOverlay::drawFunc(GtkDrawingArea *, cairo_t *cr, int width, int height,
                            gpointer userData) {
    auto *self = static_cast<CropOverlay *>(userData);
    if (!self->pixbuf_ || !self->has_selection_)
        return;

    double layoutX, layoutY, layoutW, layoutH;
    self->computeContainLayout(width, height, layoutX, layoutY, layoutW, layoutH);
    if (layoutW <= 0 || layoutH <= 0)
        return;

    double imgW = gdk_pixbuf_get_width(self->pixbuf_);
    double scale = layoutW / imgW;

    double selMinX = std::min(self->sel_x0_, self->sel_x1_);
    double selMaxX = std::max(self->sel_x0_, self->sel_x1_);
    double selMinY = std::min(self->sel_y0_, self->sel_y1_);
    double selMaxY = std::max(self->sel_y0_, self->sel_y1_);

    double rx = layoutX + selMinX * scale;
    double ry = layoutY + selMinY * scale;
    double rw = (selMaxX - selMinX) * scale;
    double rh = (selMaxY - selMinY) * scale;

    // Dim everything, then punch a clear hole over the selection.
    cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(cr, rx, ry, rw, rh);
    cairo_fill(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_set_line_width(cr, 1.5);
    cairo_rectangle(cr, rx, ry, rw, rh);
    cairo_stroke(cr);
}

void CropOverlay::applyCrop() {
    if (!has_selection_ || !pixbuf_)
        return;

    double minX = std::min(sel_x0_, sel_x1_);
    double minY = std::min(sel_y0_, sel_y1_);
    double maxX = std::max(sel_x0_, sel_x1_);
    double maxY = std::max(sel_y0_, sel_y1_);

    int imgW = gdk_pixbuf_get_width(pixbuf_);
    int imgH = gdk_pixbuf_get_height(pixbuf_);

    int x = std::clamp(static_cast<int>(std::round(minX)), 0, imgW - 1);
    int y = std::clamp(static_cast<int>(std::round(minY)), 0, imgH - 1);
    int w = std::clamp(static_cast<int>(std::round(maxX - minX)), 1, imgW - x);
    int h = std::clamp(static_cast<int>(std::round(maxY - minY)), 1, imgH - y);

    GdkPixbuf *cropped = gdk_pixbuf_new_subpixbuf(pixbuf_, x, y, w, h);
    if (!cropped)
        return;

    g_object_unref(pixbuf_);
    pixbuf_ = cropped;
    has_selection_ = false;
    refreshPicture();
}

void CropOverlay::resetCrop() {
    if (pixbuf_)
        g_object_unref(pixbuf_);
    pixbuf_ = original_ ? GDK_PIXBUF(g_object_ref(original_)) : nullptr;
    has_selection_ = false;
    refreshPicture();
}
