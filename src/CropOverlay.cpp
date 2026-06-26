#include "CropOverlay.h"

#include <pango/pangocairo.h>
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
    g_signal_connect(drag_gesture_, "drag-begin",  G_CALLBACK(onDragBegin),  this);
    g_signal_connect(drag_gesture_, "drag-update", G_CALLBACK(onDragUpdate), this);
    g_signal_connect(drag_gesture_, "drag-end",    G_CALLBACK(onDragEnd),    this);
    gtk_widget_add_controller(draw_area_, GTK_EVENT_CONTROLLER(drag_gesture_));
}

CropOverlay::~CropOverlay() {
    if (pixbuf_)   g_object_unref(pixbuf_);
    if (filtered_) g_object_unref(filtered_);
    if (original_) g_object_unref(original_);
}

void CropOverlay::setFilterEngine(FilterEngine *engine) {
    engine_ = engine;
}

void CropOverlay::setImage(GdkPixbuf *pixbuf) {
    if (pixbuf_)   { g_object_unref(pixbuf_);   pixbuf_   = nullptr; }
    if (filtered_) { g_object_unref(filtered_); filtered_ = nullptr; }
    if (original_) { g_object_unref(original_); original_ = nullptr; }

    hasCrop_         = false;
    has_selection_   = false;
    dragging_        = false;
    dragMode_        = DragMode::None;
    dragTextIdx_     = -1;
    selectedTextIdx_ = -1;
    textBoundsCache_.clear();

    if (!pixbuf) { refreshPicture(); return; }

    original_ = GDK_PIXBUF(g_object_ref(pixbuf));
    reapplyFilter();
}

void CropOverlay::reapplyFilter() {
    if (filtered_) { g_object_unref(filtered_); filtered_ = nullptr; }
    if (pixbuf_)   { g_object_unref(pixbuf_);   pixbuf_   = nullptr; }

    if (!original_) { refreshPicture(); return; }

    if (engine_ && !engine_->isIdentity()) {
        filtered_ = engine_->applyPixelFilters(original_);
    } else {
        filtered_ = GDK_PIXBUF(g_object_ref(original_));
    }

    if (hasCrop_) {
        int fw = gdk_pixbuf_get_width(filtered_);
        int fh = gdk_pixbuf_get_height(filtered_);
        int cx = std::clamp(cropX_, 0, fw - 1);
        int cy = std::clamp(cropY_, 0, fh - 1);
        int cw = std::clamp(cropW_, 1, fw - cx);
        int ch = std::clamp(cropH_, 1, fh - cy);
        pixbuf_ = gdk_pixbuf_new_subpixbuf(filtered_, cx, cy, cw, ch);
    } else {
        pixbuf_ = GDK_PIXBUF(g_object_ref(filtered_));
    }

    refreshPicture();
}

GdkPixbuf *CropOverlay::renderFinal() const {
    if (!pixbuf_) return nullptr;

    bool hasText = engine_ && !engine_->texts.empty();
    if (!hasText)
        return GDK_PIXBUF(g_object_ref(pixbuf_));

    // Ensure RGBA so bakeText can write alpha
    GdkPixbuf *result = gdk_pixbuf_get_has_alpha(pixbuf_)
                        ? gdk_pixbuf_copy(pixbuf_)
                        : gdk_pixbuf_add_alpha(pixbuf_, FALSE, 0, 0, 0);
    if (!result) return GDK_PIXBUF(g_object_ref(pixbuf_));

    int fullW = original_ ? gdk_pixbuf_get_width(original_)  : gdk_pixbuf_get_width(pixbuf_);
    int fullH = original_ ? gdk_pixbuf_get_height(original_) : gdk_pixbuf_get_height(pixbuf_);
    engine_->bakeText(result, fullW, fullH,
                      hasCrop_ ? cropX_ : 0,
                      hasCrop_ ? cropY_ : 0);
    return result;
}

void CropOverlay::setSelectedTextIdx(int idx) {
    selectedTextIdx_ = idx;
    gtk_widget_queue_draw(draw_area_);
}

void CropOverlay::applyCrop() {
    if (!has_selection_ || !filtered_) return;

    double minX = std::min(sel_x0_, sel_x1_);
    double minY = std::min(sel_y0_, sel_y1_);
    double maxX = std::max(sel_x0_, sel_x1_);
    double maxY = std::max(sel_y0_, sel_y1_);

    int fw = gdk_pixbuf_get_width(filtered_);
    int fh = gdk_pixbuf_get_height(filtered_);

    cropX_ = std::clamp(static_cast<int>(std::round(minX)), 0, fw - 1);
    cropY_ = std::clamp(static_cast<int>(std::round(minY)), 0, fh - 1);
    cropW_ = std::clamp(static_cast<int>(std::round(maxX - minX)), 1, fw - cropX_);
    cropH_ = std::clamp(static_cast<int>(std::round(maxY - minY)), 1, fh - cropY_);
    hasCrop_ = true;

    if (pixbuf_) g_object_unref(pixbuf_);
    pixbuf_ = gdk_pixbuf_new_subpixbuf(filtered_, cropX_, cropY_, cropW_, cropH_);
    has_selection_ = false;
    refreshPicture();
}

void CropOverlay::resetCrop() {
    hasCrop_ = false;
    if (pixbuf_) g_object_unref(pixbuf_);
    pixbuf_ = filtered_ ? GDK_PIXBUF(g_object_ref(filtered_)) : nullptr;
    has_selection_ = false;
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

void CropOverlay::computeContainLayout(int widgetW, int widgetH,
                                        double &x, double &y, double &w, double &h) const {
    x = y = w = h = 0;
    if (!pixbuf_ || widgetW <= 0 || widgetH <= 0) return;

    double imgW = gdk_pixbuf_get_width(pixbuf_);
    double imgH = gdk_pixbuf_get_height(pixbuf_);
    if (imgW <= 0 || imgH <= 0) return;

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

    if (w <= 0 || h <= 0 || !pixbuf_) { ix = iy = 0; return; }

    double imgW = gdk_pixbuf_get_width(pixbuf_);
    double imgH = gdk_pixbuf_get_height(pixbuf_);
    double scale = w / imgW;

    ix = std::clamp((wx - x) / scale, 0.0, imgW);
    iy = std::clamp((wy - y) / scale, 0.0, imgH);
}

int CropOverlay::hitTestText(double wx, double wy) const {
    for (int i = static_cast<int>(textBoundsCache_.size()) - 1; i >= 0; i--) {
        const auto &b = textBoundsCache_[i];
        if (wx >= b.x && wx <= b.x + b.w && wy >= b.y && wy <= b.y + b.h)
            return i;
    }
    return -1;
}

// --- draw function ---

void CropOverlay::drawFunc(GtkDrawingArea *, cairo_t *cr, int width, int height,
                            gpointer userData) {
    auto *self = static_cast<CropOverlay *>(userData);

    double layoutX, layoutY, layoutW, layoutH;
    self->computeContainLayout(width, height, layoutX, layoutY, layoutW, layoutH);

    // --- draw text overlays ---
    self->textBoundsCache_.clear();

    if (self->engine_ && self->original_ && !self->engine_->texts.empty()) {
        int fullW = gdk_pixbuf_get_width(self->original_);
        int fullH = gdk_pixbuf_get_height(self->original_);
        int pxW   = self->pixbuf_ ? gdk_pixbuf_get_width(self->pixbuf_)  : fullW;
        int pxH   = self->pixbuf_ ? gdk_pixbuf_get_height(self->pixbuf_) : fullH;
        double scale = (pxW > 0 && layoutW > 0) ? layoutW / pxW : 1.0;

        int offX = self->hasCrop_ ? self->cropX_ : 0;
        int offY = self->hasCrop_ ? self->cropY_ : 0;

        PangoLayout *layout = pango_cairo_create_layout(cr);

        for (int i = 0; i < static_cast<int>(self->engine_->texts.size()); i++) {
            const auto &t = self->engine_->texts[i];
            if (t.text.empty() || !t.visible) {
                self->textBoundsCache_.push_back({0, 0, 0, 0});
                continue;
            }

            PangoFontDescription *desc = pango_font_description_new();
            pango_font_description_set_size(desc,
                static_cast<gint>(t.fontSize * scale * PANGO_SCALE));
            pango_layout_set_font_description(layout, desc);
            pango_font_description_free(desc);
            pango_layout_set_text(layout, t.text.c_str(), -1);

            int lw, lh;
            pango_layout_get_pixel_size(layout, &lw, &lh);

            // position in filtered_ image coords → pixbuf_ coords → widget coords
            float fx = t.normX * fullW - offX;
            float fy = t.normY * fullH - offY;
            float wx = static_cast<float>(layoutX + fx * scale);
            float wy = static_cast<float>(layoutY + fy * scale);

            float tx = wx - lw * 0.5f;
            float ty = wy - lh * 0.5f;

            self->textBoundsCache_.push_back({tx - 2, ty - 2,
                                               static_cast<float>(lw + 4),
                                               static_cast<float>(lh + 4)});

            cairo_set_source_rgba(cr, t.r, t.g, t.b, t.a);
            cairo_move_to(cr, tx, ty);
            pango_cairo_show_layout(cr, layout);

            if (i == self->selectedTextIdx_) {
                cairo_set_source_rgba(cr, 1.0, 0.85, 0.0, 0.8);
                cairo_set_line_width(cr, 1.5);
                cairo_rectangle(cr, tx - 2, ty - 2, lw + 4, lh + 4);
                cairo_stroke(cr);
            }
        }

        g_object_unref(layout);
    }

    // --- draw crop selection ---
    if (!self->pixbuf_ || !self->has_selection_) return;
    if (layoutW <= 0 || layoutH <= 0) return;

    double imgW  = gdk_pixbuf_get_width(self->pixbuf_);
    double scale2 = layoutW / imgW;

    double selMinX = std::min(self->sel_x0_, self->sel_x1_);
    double selMaxX = std::max(self->sel_x0_, self->sel_x1_);
    double selMinY = std::min(self->sel_y0_, self->sel_y1_);
    double selMaxY = std::max(self->sel_y0_, self->sel_y1_);

    double rx = layoutX + selMinX * scale2;
    double ry = layoutY + selMinY * scale2;
    double rw = (selMaxX - selMinX) * scale2;
    double rh = (selMaxY - selMinY) * scale2;

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

// --- gesture handlers ---

void CropOverlay::onDragBegin(GtkGestureDrag *, double x, double y, gpointer userData) {
    auto *self = static_cast<CropOverlay *>(userData);
    if (!self->pixbuf_) return;

    int textHit = self->hitTestText(x, y);
    if (textHit >= 0) {
        self->dragMode_        = DragMode::TextMove;
        self->dragTextIdx_     = textHit;
        self->selectedTextIdx_ = textHit;
        const auto &t = self->engine_->texts[textHit];
        self->dragTextStartNX_ = t.normX;
        self->dragTextStartNY_ = t.normY;
        self->dragTextStartPx_ = static_cast<float>(x);
        self->dragTextStartPy_ = static_cast<float>(y);
        gtk_widget_queue_draw(self->draw_area_);
        return;
    }

    self->dragMode_     = DragMode::CropDraw;
    self->dragging_     = true;
    self->widgetToImage(x, y, self->sel_x0_, self->sel_y0_);
    self->sel_x1_       = self->sel_x0_;
    self->sel_y1_       = self->sel_y0_;
    self->has_selection_ = true;
    gtk_widget_queue_draw(self->draw_area_);
}

void CropOverlay::onDragUpdate(GtkGestureDrag *gesture, double, double, gpointer userData) {
    auto *self = static_cast<CropOverlay *>(userData);

    if (self->dragMode_ == DragMode::TextMove && self->dragTextIdx_ >= 0
        && self->engine_
        && self->dragTextIdx_ < static_cast<int>(self->engine_->texts.size())) {

        double offsetX, offsetY;
        gtk_gesture_drag_get_offset(gesture, &offsetX, &offsetY);

        // Convert pixel drag offset → normalized image delta
        int widgetW = gtk_widget_get_width(self->draw_area_);
        int widgetH = gtk_widget_get_height(self->draw_area_);
        double lx, ly, lw, lh;
        self->computeContainLayout(widgetW, widgetH, lx, ly, lw, lh);

        int fullW = self->original_ ? gdk_pixbuf_get_width(self->original_)  : 1;
        int fullH = self->original_ ? gdk_pixbuf_get_height(self->original_) : 1;
        int pxW   = self->pixbuf_   ? gdk_pixbuf_get_width(self->pixbuf_)    : fullW;
        double scale = (pxW > 0 && lw > 0) ? lw / pxW : 1.0;

        float dnx = static_cast<float>(offsetX / scale / fullW);
        float dny = static_cast<float>(offsetY / scale / fullH);

        auto &t = self->engine_->texts[self->dragTextIdx_];
        t.normX = std::clamp(self->dragTextStartNX_ + dnx, 0.0f, 1.0f);
        t.normY = std::clamp(self->dragTextStartNY_ + dny, 0.0f, 1.0f);

        gtk_widget_queue_draw(self->draw_area_);
        return;
    }

    if (self->dragMode_ == DragMode::CropDraw && self->dragging_) {
        double startX, startY, offsetX, offsetY;
        gtk_gesture_drag_get_start_point(gesture, &startX, &startY);
        gtk_gesture_drag_get_offset(gesture, &offsetX, &offsetY);
        self->widgetToImage(startX + offsetX, startY + offsetY, self->sel_x1_, self->sel_y1_);
        gtk_widget_queue_draw(self->draw_area_);
    }
}

void CropOverlay::onDragEnd(GtkGestureDrag *gesture, double, double, gpointer userData) {
    auto *self = static_cast<CropOverlay *>(userData);

    if (self->dragMode_ == DragMode::TextMove) {
        self->dragMode_    = DragMode::None;
        self->dragTextIdx_ = -1;
        return;
    }

    if (self->dragMode_ == DragMode::CropDraw && self->dragging_) {
        self->dragging_ = false;
        self->dragMode_ = DragMode::None;

        double startX, startY, offsetX, offsetY;
        gtk_gesture_drag_get_start_point(gesture, &startX, &startY);
        gtk_gesture_drag_get_offset(gesture, &offsetX, &offsetY);
        self->widgetToImage(startX + offsetX, startY + offsetY, self->sel_x1_, self->sel_y1_);

        if (std::abs(self->sel_x1_ - self->sel_x0_) < 2 ||
            std::abs(self->sel_y1_ - self->sel_y0_) < 2)
            self->has_selection_ = false;

        gtk_widget_queue_draw(self->draw_area_);
    }
}
