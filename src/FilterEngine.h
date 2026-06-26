#pragma once

#include <gtk/gtk.h>
#include <string>
#include <vector>

struct TextOverlay {
    std::string text;
    float fontSize = 24.0f;
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
    float normX = 0.5f, normY = 0.5f;  // 0..1 in full (pre-crop) image space
    bool  visible = true;
};

class FilterEngine {
public:
    float brightness     = 0.0f;   // -1.0 to +1.0
    float saturation     = 1.0f;   // 0.0 = gray, 1.0 = neutral, 2.0 = double
    float vignette       = 0.0f;   // 0.0 = off, 1.0 = max
    int   pixelate       = 0;      // 0 = off, block size in pixels
    bool  lumaKeyEnabled = false;
    float lumaKey        = 0.5f;   // threshold (0..1)
    bool  lumaInvert     = false;  // true: transparent above threshold

    std::vector<TextOverlay> texts;

    bool needsAlpha() const { return lumaKeyEnabled; }

    bool isIdentity() const {
        return brightness == 0.0f && saturation == 1.0f && vignette == 0.0f &&
               pixelate == 0 && !lumaKeyEnabled;
    }

    // Returns new GdkPixbuf with pixel filters applied; caller owns the ref.
    GdkPixbuf *applyPixelFilters(GdkPixbuf *src) const;

    // Bakes all TextOverlays into dst in-place via Cairo+Pango.
    // srcFullW/H are the full pre-crop image dimensions (for normX/normY conversion).
    // cropOffX/Y are the crop origin within the full image (0 if uncropped).
    void bakeText(GdkPixbuf *dst, int srcFullW, int srcFullH,
                  int cropOffX = 0, int cropOffY = 0) const;

    void reset();

private:
    static void runBrightness(guchar *px, int w, int h, int stride, int ch, float v);
    static void runSaturation(guchar *px, int w, int h, int stride, int ch, float v);
    static void runLumaKey   (guchar *px, int w, int h, int stride, float threshold, bool invert);
    static void runPixelate  (guchar *px, int w, int h, int stride, int ch, int block);
    static void runVignette  (guchar *px, int w, int h, int stride, int ch, float strength);
};
