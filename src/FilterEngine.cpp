#include "FilterEngine.h"

#include <pango/pangocairo.h>
#include <algorithm>
#include <cmath>

static guchar clampByte(float v) {
    return static_cast<guchar>(std::clamp(v, 0.0f, 255.0f));
}

void FilterEngine::reset() {
    brightness     = 0.0f;
    saturation     = 1.0f;
    vignette       = 0.0f;
    pixelate       = 0;
    lumaKeyEnabled = false;
    lumaKey        = 0.5f;
    lumaInvert     = false;
    texts.clear();
}

// --- pixel filter primitives ---

void FilterEngine::runBrightness(guchar *px, int w, int h, int stride, int ch, float v) {
    float delta = v * 255.0f;
    for (int y = 0; y < h; y++) {
        guchar *row = px + y * stride;
        for (int x = 0; x < w; x++) {
            guchar *p = row + x * ch;
            p[0] = clampByte(p[0] + delta);
            p[1] = clampByte(p[1] + delta);
            p[2] = clampByte(p[2] + delta);
        }
    }
}

void FilterEngine::runSaturation(guchar *px, int w, int h, int stride, int ch, float sat) {
    for (int y = 0; y < h; y++) {
        guchar *row = px + y * stride;
        for (int x = 0; x < w; x++) {
            guchar *p = row + x * ch;
            float r = p[0], g = p[1], b = p[2];
            float luma = 0.299f * r + 0.587f * g + 0.114f * b;
            p[0] = clampByte(luma + sat * (r - luma));
            p[1] = clampByte(luma + sat * (g - luma));
            p[2] = clampByte(luma + sat * (b - luma));
        }
    }
}

void FilterEngine::runLumaKey(guchar *px, int w, int h, int stride, float threshold, bool invert) {
    float thresh = threshold * 255.0f;
    for (int y = 0; y < h; y++) {
        guchar *row = px + y * stride;
        for (int x = 0; x < w; x++) {
            guchar *p = row + x * 4;
            float luma = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
            bool transparent = invert ? (luma > thresh) : (luma < thresh);
            if (transparent)
                p[3] = 0;
        }
    }
}

void FilterEngine::runPixelate(guchar *px, int w, int h, int stride, int ch, int block) {
    for (int by = 0; by < h; by += block) {
        for (int bx = 0; bx < w; bx += block) {
            int bw = std::min(block, w - bx);
            int bh = std::min(block, h - by);
            float sum[4] = {};
            int count = bw * bh;
            for (int dy = 0; dy < bh; dy++) {
                guchar *row = px + (by + dy) * stride + bx * ch;
                for (int dx = 0; dx < bw; dx++) {
                    guchar *p = row + dx * ch;
                    for (int c = 0; c < ch && c < 4; c++)
                        sum[c] += p[c];
                }
            }
            guchar avg[4];
            for (int c = 0; c < ch && c < 4; c++)
                avg[c] = clampByte(sum[c] / count);
            for (int dy = 0; dy < bh; dy++) {
                guchar *row = px + (by + dy) * stride + bx * ch;
                for (int dx = 0; dx < bw; dx++) {
                    guchar *p = row + dx * ch;
                    for (int c = 0; c < ch && c < 4; c++)
                        p[c] = avg[c];
                }
            }
        }
    }
}

void FilterEngine::runVignette(guchar *px, int w, int h, int stride, int ch, float strength) {
    float cx = w * 0.5f, cy = h * 0.5f;
    float maxDist = std::sqrt(cx * cx + cy * cy);
    if (maxDist < 1.0f) return;
    for (int y = 0; y < h; y++) {
        guchar *row = px + y * stride;
        for (int x = 0; x < w; x++) {
            float dx = (x - cx) / maxDist;
            float dy = (y - cy) / maxDist;
            float factor = 1.0f - strength * (dx * dx + dy * dy);
            factor = std::max(factor, 0.0f);
            guchar *p = row + x * ch;
            p[0] = clampByte(p[0] * factor);
            p[1] = clampByte(p[1] * factor);
            p[2] = clampByte(p[2] * factor);
        }
    }
}

GdkPixbuf *FilterEngine::applyPixelFilters(GdkPixbuf *src) const {
    if (!src) return nullptr;

    int w       = gdk_pixbuf_get_width(src);
    int h       = gdk_pixbuf_get_height(src);
    bool srcAlpha  = gdk_pixbuf_get_has_alpha(src);
    bool wantAlpha = srcAlpha || needsAlpha();

    GdkPixbuf *dst = wantAlpha && !srcAlpha
                     ? gdk_pixbuf_add_alpha(src, FALSE, 0, 0, 0)
                     : gdk_pixbuf_copy(src);
    if (!dst) return nullptr;

    int ch     = gdk_pixbuf_get_n_channels(dst);
    int stride = gdk_pixbuf_get_rowstride(dst);
    guchar *px = gdk_pixbuf_get_pixels(dst);

    if (brightness != 0.0f)  runBrightness(px, w, h, stride, ch, brightness);
    if (saturation != 1.0f)  runSaturation(px, w, h, stride, ch, saturation);
    if (lumaKeyEnabled)      runLumaKey(px, w, h, stride, lumaKey, lumaInvert);
    if (pixelate > 1)        runPixelate(px, w, h, stride, ch, pixelate);
    if (vignette > 0.0f)     runVignette(px, w, h, stride, ch, vignette);

    return dst;
}

void FilterEngine::bakeText(GdkPixbuf *dst, int srcFullW, int srcFullH,
                             int cropOffX, int cropOffY) const {
    if (texts.empty() || !dst) return;

    int w = gdk_pixbuf_get_width(dst);
    int h = gdk_pixbuf_get_height(dst);
    int srcCh  = gdk_pixbuf_get_n_channels(dst);
    int stride = gdk_pixbuf_get_rowstride(dst);
    guchar *srcPx = gdk_pixbuf_get_pixels(dst);

    // Copy pixbuf → premultiplied ARGB32 Cairo surface
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    guchar *surfData = cairo_image_surface_get_data(surf);
    int surfStride   = cairo_image_surface_get_stride(surf);

    cairo_surface_flush(surf);
    for (int y = 0; y < h; y++) {
        const guchar *srcRow = srcPx + y * stride;
        guint32 *dstRow = reinterpret_cast<guint32 *>(surfData + y * surfStride);
        for (int x = 0; x < w; x++) {
            const guchar *p = srcRow + x * srcCh;
            guchar r = p[0], g = p[1], b = p[2];
            guchar a = srcCh >= 4 ? p[3] : 255u;
            // premultiply
            r = static_cast<guchar>((guint32)r * a / 255u);
            g = static_cast<guchar>((guint32)g * a / 255u);
            b = static_cast<guchar>((guint32)b * a / 255u);
            dstRow[x] = ((guint32)a << 24) | ((guint32)r << 16) | ((guint32)g << 8) | b;
        }
    }
    cairo_surface_mark_dirty(surf);

    // Draw text overlays
    cairo_t *cr = cairo_create(surf);
    PangoLayout *layout = pango_cairo_create_layout(cr);

    for (const auto &t : texts) {
        if (t.text.empty() || !t.visible) continue;

        PangoFontDescription *desc = pango_font_description_new();
        pango_font_description_set_size(desc, static_cast<gint>(t.fontSize * PANGO_SCALE));
        pango_layout_set_font_description(layout, desc);
        pango_font_description_free(desc);
        pango_layout_set_text(layout, t.text.c_str(), -1);

        int lw, lh;
        pango_layout_get_pixel_size(layout, &lw, &lh);

        // text anchor in full image coords
        float fx = t.normX * srcFullW - cropOffX;
        float fy = t.normY * srcFullH - cropOffY;
        float tx = fx - lw * 0.5f;
        float ty = fy - lh * 0.5f;

        cairo_set_source_rgba(cr, t.r, t.g, t.b, t.a);
        cairo_move_to(cr, tx, ty);
        pango_cairo_show_layout(cr, layout);
    }

    g_object_unref(layout);
    cairo_destroy(cr);

    // Copy Cairo surface back → GdkPixbuf (un-premultiply)
    cairo_surface_flush(surf);
    for (int y = 0; y < h; y++) {
        guchar *dstRow = srcPx + y * stride;
        const guint32 *surfRow = reinterpret_cast<const guint32 *>(surfData + y * surfStride);
        for (int x = 0; x < w; x++) {
            guchar *p = dstRow + x * srcCh;
            guint32 pixel = surfRow[x];
            guchar a = (pixel >> 24) & 0xFF;
            guchar r = (pixel >> 16) & 0xFF;
            guchar g = (pixel >>  8) & 0xFF;
            guchar b = (pixel      ) & 0xFF;
            if (a > 0) {
                r = static_cast<guchar>((guint32)r * 255u / a);
                g = static_cast<guchar>((guint32)g * 255u / a);
                b = static_cast<guchar>((guint32)b * 255u / a);
            }
            p[0] = r; p[1] = g; p[2] = b;
            if (srcCh >= 4) p[3] = a;
        }
    }
    cairo_surface_destroy(surf);
}
