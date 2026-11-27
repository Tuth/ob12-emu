// The front panel for macOS: the same vector art, controls, LCD and lamps as panel_view.cpp, drawn
// with Core Graphics in an NSView, and the Audio Unit's Cocoa view factory. The art handling
// (loading, recolouring, finding the moving parts) is panel_view.cpp's, kept line for line; only
// the drawing and the window plumbing differ.
#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#if __has_include(<AudioToolbox/AUCocoaUIView.h>)
#import <AudioToolbox/AUCocoaUIView.h>
#else
#import <AudioUnit/AUCocoaUIView.h>
#endif
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <vector>
#include "ob12_engine.h"
#include "panel_controls.h"
#include "panel_view.h"
#include "ob12_mac.h"

#ifndef OB12_VIEW_FACTORY
#define OB12_VIEW_FACTORY OB12_AUViewFactory_0A00
#endif
#ifndef OB12_PANEL_CLASS
#define OB12_PANEL_CLASS OB12_PanelView_0A00
#endif
#ifndef OB12_LCD_CLASS
#define OB12_LCD_CLASS OB12_LCDView_0A00
#endif

namespace {

struct Run {
    uint32_t argb;
    uint8_t mode;  // 1 fill (no antialias), 2 fill even-odd, 3 stroke, 4 push clip, 5 pop clip
    float width;
    std::vector<uint8_t> kinds;
    std::vector<float> points;
    CGPathRef path = nullptr;
    CGRect bounds = CGRectNull;  // art units, with the stroke: lets the magnifier skip what it does not show
    bool moving = false;
};

struct Moving {
    std::vector<int> runs;
    float cx = 0, cy = 0, rest = 0, travel = 0, lo = -135, hi = 135, radius = 0;
    float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    bool found = false;
};

const int scale_percents[] = {50, 67, 75, 100, 125, 150, 200};
const uint32_t panel_blue = 0xff2251b0, panel_cream = 0xffece5c6;
const int min_width = 600;

void set_fill(CGContextRef c, uint32_t argb, float alpha = 1) {
    CGContextSetRGBFillColor(c, ((argb >> 16) & 255) / 255.f, ((argb >> 8) & 255) / 255.f, (argb & 255) / 255.f, (argb >> 24) / 255.f * alpha);
}
void set_stroke(CGContextRef c, uint32_t argb) {
    CGContextSetRGBStrokeColor(c, ((argb >> 16) & 255) / 255.f, ((argb >> 8) & 255) / 255.f, (argb & 255) / 255.f, (argb >> 24) / 255.f);
}

std::atomic<int> gain_db{-1};
std::string gain_file() { return ob12mac::support_folder(true) + "/output_gain.txt"; }
int read_gain() {
    int db = 0;
    if (FILE* f = fopen(gain_file().c_str(), "r")) { if (fscanf(f, "%d", &db) != 1) db = 0; fclose(f); }
    return std::max(0, std::min(24, db));
}
void write_gain(int db) {
    gain_db = db;
    if (FILE* f = fopen(gain_file().c_str(), "w")) { fprintf(f, "%d\n", db); fclose(f); }
}

std::string width_file() { return ob12mac::support_folder(true) + "/editor_width.txt"; }
int load_width() {
    int width = PanelView::base_width;
    if (FILE* f = fopen(width_file().c_str(), "r")) { if (fscanf(f, "%d", &width) != 1) width = PanelView::base_width; fclose(f); }
    return std::clamp(width, min_width, 6000);
}
void save_width(int width) {
    if (FILE* f = fopen(width_file().c_str(), "w")) { fprintf(f, "%d\n", width); fclose(f); }
}

// The magnifier: on or off, its zoom and its diameter on the screen in millimetres. Per user.
const int strip_height = 30;  // the bar under the panel that holds the magnifier's button
const float loupe_zooms[3] = {1.5f, 2.f, 2.5f}, loupe_sizes[4] = {30, 35, 40, 50};
struct LoupeSettings { bool on = false; float zoom = 2.5f, mm = 35; };
std::string loupe_file() { return ob12mac::support_folder(true) + "/magnifier.txt"; }
LoupeSettings load_loupe() {
    LoupeSettings l;
    if (FILE* f = fopen(loupe_file().c_str(), "r")) {
        int on = 0; float zoom = 0, mm = 0;
        if (fscanf(f, "%d %f %f", &on, &zoom, &mm) == 3) {
            l.on = on != 0;
            if (zoom >= 1.5f && zoom <= 2.5f) l.zoom = zoom;  // a 3x or 4x saved earlier comes back as 2.5x
            else if (zoom > 2.5f) l.zoom = 2.5f;
            if (mm >= 30 && mm <= 50) l.mm = mm;
            else if (mm > 50) l.mm = 50;
        }
        fclose(f);
    }
    return l;
}
std::string lcd_window_file() { return ob12mac::support_folder(true) + "/lcd_window.txt"; }
bool load_lcd_window() {
    int on = 0;
    if (FILE* f = fopen(lcd_window_file().c_str(), "r")) { if (fscanf(f, "%d", &on) != 1) on = 0; fclose(f); }
    return on != 0;
}
void save_lcd_window(bool on) {
    if (FILE* f = fopen(lcd_window_file().c_str(), "w")) { fprintf(f, "%d\n", on ? 1 : 0); fclose(f); }
}
void save_loupe(const LoupeSettings& l) {
    if (FILE* f = fopen(loupe_file().c_str(), "w")) { fprintf(f, "%d %.2f %.1f\n", l.on ? 1 : 0, l.zoom, l.mm); fclose(f); }
}

// Draw an image or layer made in y-up space into the flipped view so it stands upright.
void draw_upright_image(CGContextRef c, CGImageRef image, CGRect r) {
    CGContextSaveGState(c);
    CGContextTranslateCTM(c, r.origin.x, r.origin.y + r.size.height);
    CGContextScaleCTM(c, 1, -1);
    CGContextDrawImage(c, CGRectMake(0, 0, r.size.width, r.size.height), image);
    CGContextRestoreGState(c);
}

}  // namespace

// The art and its state: panel_view.cpp's Impl without Direct2D.
struct PanelArt {
    Engine& engine;
    std::vector<Run> runs;
    float art_w = 472, art_h = 126;
    int width = 0, height = 0;  // points
    int pot_value[sizeof panel_pots / sizeof panel_pots[0]];
    Moving moving[sizeof panel_pots / sizeof panel_pots[0]];
    Moving dial;
    float dial_angle = 0;
    int drag_pot = -1, drag_start_y = 0, drag_start_value = 0, down_key = -1;
    bool drag_dial = false;
    float dial_grab = 0, dial_rest = 0;
    uint8_t lcd_bits[30 * 64];
    uint8_t lcd_shown[30 * 64] = {};
    bool lcd_valid = false;
    bool lcd_have = false;  // lcd_shown holds a real frame (the magnifier draws from it)
    bool lcd_pending = false;  // lcd_shown changed since the LCD bitmap was made
    uint8_t leds_shown[16] = {};

    // Asked by the clock, so that only what changed is redrawn.
    bool poll_lcd() {
        if (!engine.lcd(lcd_bits)) return false;
        if (lcd_have && memcmp(lcd_bits, lcd_shown, sizeof lcd_bits) == 0) return false;
        memcpy(lcd_shown, lcd_bits, sizeof lcd_bits);
        lcd_have = lcd_pending = true;
        return true;
    }
    // The lamps that changed, as an area in view units (null: none). A blinking tempo lamp
    // redrew the whole panel at each blink.
    CGRect poll_leds() {
        uint8_t now[16];
        engine.leds(now);
        if (memcmp(now, leds_shown, sizeof now) == 0) return CGRectNull;
        float s = sx();
        CGRect area = CGRectNull;
        for (const PanelLed& led : panel_leds) {
            if ((now[led.latch] >> led.bit & 1) == (leds_shown[led.latch] >> led.bit & 1)) continue;
            float r = (led.lamp ? 5.6f : 3.1f) * 1.6f * s + 3;
            area = CGRectUnion(area, CGRectMake(left() + led.x * s - r, led.y * s - r, 2 * r, 2 * r));
        }
        memcpy(leds_shown, now, sizeof now);
        return area;
    }
    CGImageRef lcd_image = nullptr;
    int lcd_w = 0, lcd_h = 0;
    CGLayerRef art_layer = nullptr;
    float art_backing = 0;

    explicit PanelArt(Engine& e) : engine(e) {
        for (int& v : pot_value) v = 0x80;
        memset(lcd_bits, 0, sizeof lcd_bits);
    }
    ~PanelArt() {
        for (auto& r : runs) if (r.path) CGPathRelease(r.path);
        if (lcd_image) CGImageRelease(lcd_image);
        if (art_layer) CGLayerRelease(art_layer);
        forget_zoom_layer();
    }

    void load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        std::vector<char> data((std::istreambuf_iterator<char>(f)), {});
        if (data.size() < 16 || memcmp(data.data(), "OB12", 4)) return;
        // Every read is checked against the end: a short or damaged file leaves no art, not a crash.
        const char* p = data.data() + 4;
        const char* end = data.data() + data.size();
        bool ok = true;
        auto take = [&](void* out, size_t n) {
            if (!ok || (size_t)(end - p) < n) { ok = false; memset(out, 0, n); return; }
            memcpy(out, p, n);
            p += n;
        };
        auto u8 = [&]() { uint8_t v; take(&v, 1); return v; };
        auto u32 = [&]() { uint32_t v; take(&v, 4); return v; };
        auto f32 = [&]() { float v; take(&v, 4); return v; };
        uint32_t count = u32();
        art_w = f32();
        art_h = f32();
        if (!ok || count > (size_t)(end - p) / 13) { runs.clear(); return; }  // 13: the smallest run
        runs.resize(count);
        for (auto& r : runs) {
            r.argb = u32();
            r.mode = u8();
            r.width = f32();
            uint32_t commands = u32();
            if (!ok || commands > (size_t)(end - p)) { ok = false; break; }
            r.kinds.reserve(commands);
            for (uint32_t i = 0; i < commands && ok; i++) {
                uint8_t kind = u8();
                r.kinds.push_back(kind);
                int n = kind == 2 ? 3 : kind == 3 ? 0 : 1;
                for (int j = 0; j < n * 2; j++) r.points.push_back(f32());
            }
            if (!ok) break;
        }
        if (!ok) { runs.clear(); return; }
        fix_art();
    }

    void recolour() {
        const float k = art_w / panel_frame_w;
        for (Run& r : runs) {
            if (r.mode >= 4 || r.points.empty()) continue;
            int cr = (r.argb >> 16) & 255, cg = (r.argb >> 8) & 255, cb = r.argb & 255;
            if (std::abs(cr - cg) > 12 || std::abs(cg - cb) > 12) continue;
            int v = (cr + cg + cb) / 3;
            float x0, y0, x1, y1;
            box(r, x0, y0, x1, y1);
            float w = (x1 - x0) / k, h = (y1 - y0) / k;
            int figures = (int)std::count(r.kinds.begin(), r.kinds.end(), (uint8_t)0);
            uint32_t a = r.argb & 0xff000000;
            if (r.argb == 0xff6d6e71 && r.mode == 2) { r.argb = a | 0xd6d6d4; continue; }
            if (r.argb == 0xffbcbec0 && r.mode == 2 && w < 30 && r.moving) { r.argb = a | 0xd6d6d4; continue; }
            if (v > 0xf0 && figures == 1 && w < 1 && h > 40) { r.argb = a | 0x0c0c0e; continue; }
            if (v > 0xf0) { r.argb = a | 0xdcdcd8; continue; }
            if (v < 0x40 && r.mode == 1 && !r.moving && w < 8 && h < 3) { r.argb = a | (panel_cream & 0xffffff); continue; }
            if (v < 0x40 && r.mode != 1) {
                if (r.mode == 3 && figures >= 60) { r.argb = a | 0x3a3a3c; continue; }
                bool plain_block = r.mode != 3 && r.kinds.size() <= 6 && (w > 40 || h > 40);
                r.argb = plain_block ? (a | 0x0c0c0e) : (a | (panel_cream & 0xffffff));
                continue;
            }
            int d = 0x12 + std::max(0, v - 0x30) * 30 / 100;
            if (d > 0x58) d = 0x58;
            r.argb = a | (uint32_t)(d << 16 | d << 8 | d);
        }
    }

    void fix_art() {
        const float k = art_w / panel_frame_w;
        auto outside = [&](float x, float y) { return x < 0 || y < 0 || x > art_w || y > art_h; };
        for (Run& r : runs) {
            if (r.mode != 3) continue;
            std::vector<uint8_t> kinds;
            std::vector<float> points;
            bool drop = false;
            size_t q = 0;
            for (uint8_t kind : r.kinds) {
                int n = kind == 2 ? 3 : kind == 3 ? 0 : 1;
                if (kind == 0) drop = outside(r.points[q], r.points[q + 1]);
                if (!drop) {
                    kinds.push_back(kind);
                    points.insert(points.end(), r.points.begin() + q, r.points.begin() + q + n * 2);
                }
                q += n * 2;
            }
            r.kinds.swap(kinds);
            r.points.swap(points);
        }
        for (Run& r : runs) {
            if (r.mode != 3 || r.width < 0.4f || r.points.size() < 2) continue;
            if (std::fabs(r.points[0] / k - 40.5f) > 0.5f || std::fabs(r.points[1] / k - 20.1f) > 0.5f) continue;
            r.kinds.push_back(0); r.points.push_back(40.5f * k); r.points.push_back(20.1f * k);
            r.kinds.push_back(1); r.points.push_back(186.1f * k); r.points.push_back(20.1f * k);
            break;
        }
    }

    void box(const Run& r, float& x0, float& y0, float& x1, float& y1) const {
        x0 = y0 = 1e9f; x1 = y1 = -1e9f;
        for (size_t i = 0; i + 1 < r.points.size(); i += 2) {
            x0 = std::min(x0, r.points[i]); x1 = std::max(x1, r.points[i]);
            y0 = std::min(y0, r.points[i + 1]); y1 = std::max(y1, r.points[i + 1]);
        }
    }

    void find_dial() {
        const float k = art_w / panel_frame_w;
        float sx = 0, sy = 0; int n = 0;
        for (const Run& run : runs) {
            if (run.mode != 1 || run.points.empty()) continue;
            float x0, y0, x1, y1;
            box(run, x0, y0, x1, y1);
            float w = (x1 - x0) / k, h = (y1 - y0) / k;
            if (w < 140 || w > 160 || h < 140 || h > 160) continue;
            if (std::fabs((x0 + x1) / 2 / k - 1757) > 40 || std::fabs((y0 + y1) / 2 / k - 383) > 40) continue;
            sx += (x0 + x1) / 2; sy += (y0 + y1) / 2; n++;
        }
        if (!n) return;
        dial.cx = sx / n; dial.cy = sy / n;
        for (int r = 0; r < (int)runs.size(); r++) {
            if (runs[r].mode != 4) continue;
            float x0, y0, x1, y1;
            box(runs[r], x0, y0, x1, y1);
            float w = (x1 - x0) / k;
            if (w < 28 || w > 55 || std::hypot((x0 + x1) / 2 - dial.cx, (y0 + y1) / 2 - dial.cy) > 45 * k) continue;
            int depth = 0;
            for (int q = r; q < (int)runs.size(); q++) {
                if (runs[q].mode == 4) depth++;
                if (runs[q].mode == 5) depth--;
                dial.runs.push_back(q);
                runs[q].moving = true;
                if (depth == 0) break;
            }
            dial.found = true;
            break;
        }
    }

    void find_moving() {
        const float k = art_w / panel_frame_w;
        int count = sizeof panel_pots / sizeof panel_pots[0];
        for (int i = 0; i < count; i++) {
            const PanelPot& p = panel_pots[i];
            Moving& m = moving[i];
            float px = p.x * k, py = p.y * k;
            int first = -1, last = -1;
            std::vector<int> picked;
            if (p.slider) {
                for (int r = 0; r < (int)runs.size(); r++) {
                    if (runs[r].mode >= 4 || runs[r].points.empty()) continue;
                    float x0, y0, x1, y1;
                    box(runs[r], x0, y0, x1, y1);
                    if (x0 >= px - 14 * k && x1 <= px + 14 * k && y0 >= py + 36 * k && y1 <= py + 92 * k) picked.push_back(r);
                }
                m.travel = 88 * k;
            } else {
                int body = -1;
                float best = 1e9f;
                for (int r = 0; r < (int)runs.size(); r++) {
                    const Run& run = runs[r];
                    if (run.mode != 2 || run.argb != 0xffbcbec0) continue;
                    float x0, y0, x1, y1;
                    box(run, x0, y0, x1, y1);
                    float w = (x1 - x0) / k, h = (y1 - y0) / k;
                    if (w < 40 || w > 60 || h < 30 || h > 60) continue;
                    float d = std::hypot((x0 + x1) / 2 - px, (y0 + y1) / 2 - py);
                    if (d < best) { best = d; body = r; m.cx = (x0 + x1) / 2; m.cy = (y0 + y1) / 2; }
                }
                if (body < 0 || best > 25 * k) {
                    body = -1; best = 1e9f;
                    for (int r = 0; r < (int)runs.size(); r++) {
                        const Run& run = runs[r];
                        int figures = (int)std::count(run.kinds.begin(), run.kinds.end(), (uint8_t)0);
                        if (run.mode != 3 || figures < 60 || figures > 72) continue;
                        float x0, y0, x1, y1;
                        box(run, x0, y0, x1, y1);
                        if ((y1 - y0) / k < 30 || (y1 - y0) / k > 42) continue;
                        float d = std::hypot((x0 + x1) / 2 - px, (y0 + y1) / 2 - py);
                        if (d < best) { best = d; body = r; m.cx = (x0 + x1) / 2; m.cy = (y0 + y1) / 2; }
                    }
                    if (body < 0 || best > 25 * k) continue;
                }
                float bx0, by0, bx1, by1;
                box(runs[body], bx0, by0, bx1, by1);
                float radius = (by1 - by0) / 2;
                m.radius = radius;
                double sx = 0, sy = 0;
                long sn = 0;
                for (int r = 0; r < (int)runs.size(); r++) {
                    const Run& run = runs[r];
                    if (r == body || run.mode >= 4 || run.points.empty()) continue;
                    int figures = (int)std::count(run.kinds.begin(), run.kinds.end(), (uint8_t)0);
                    if (figures > 8) continue;
                    bool inside = true;
                    for (size_t j = 0; j + 1 < run.points.size() && inside; j += 2)
                        if (std::hypot(run.points[j] - m.cx, run.points[j + 1] - m.cy) > radius * 1.02f) inside = false;
                    if (!inside) continue;
                    picked.push_back(r);
                    if (run.mode == 2 && run.argb == 0xff6d6e71) for (size_t j = 0; j + 1 < run.points.size(); j += 2) { sx += run.points[j] - m.cx; sy += run.points[j + 1] - m.cy; sn++; }
                }
                for (int r = 0; r < (int)runs.size(); r++) {
                    const Run& run = runs[r];
                    if (run.mode != 2 || run.argb != 0xff231f20 || run.points.empty()) continue;
                    float x0, y0, x1, y1;
                    box(run, x0, y0, x1, y1);
                    if ((x1 - x0) / k > 9 || (y1 - y0) / k > 9) continue;
                    float dx = (x0 + x1) / 2 - m.cx, dy = (y0 + y1) / 2 - m.cy, d = std::hypot(dx, dy);
                    if (d < radius * 1.1f || d > radius * 1.9f || dy <= 0) continue;
                    float a = atan2f(dx, -dy) * 180 / 3.14159265f;
                    if (a < -120) m.lo = std::max(m.lo == -135 ? -180.f : m.lo, a);
                    if (a > 120) m.hi = std::min(m.hi == 135 ? 180.f : m.hi, a);
                }
                if (m.lo <= -180 || m.hi >= 180) { m.lo = -135; m.hi = 135; }
                if (!sn) for (int r : picked) if (runs[r].mode == 2)
                    for (size_t j = 0; j + 1 < runs[r].points.size(); j += 2) { sx += runs[r].points[j] - m.cx; sy += runs[r].points[j + 1] - m.cy; sn++; }
                if (sn) m.rest = (float)(atan2(sx, -sy) * 180 / 3.14159265);
            }
            if (picked.empty()) continue;
            first = picked.front(); last = picked.back();
            for (int r : picked) { runs[r].moving = true; m.runs.push_back(r); }
            std::vector<int> open;
            for (int r = first; r <= last; r++) {
                if (runs[r].mode == 4) open.push_back(r);
                else if (runs[r].mode == 5 && !open.empty()) {
                    int push = open.back();
                    open.pop_back();
                    bool whole = true;
                    for (int q = push + 1; q < r; q++) if (!runs[q].moving && runs[q].mode < 4) whole = false;
                    if (!whole) continue;
                    runs[push].moving = runs[r].moving = true;
                    m.runs.push_back(push);
                    m.runs.push_back(r);
                }
            }
            std::sort(m.runs.begin(), m.runs.end());
            if (p.slider) {
                m.bx0 = m.by0 = 1e9f; m.bx1 = m.by1 = -1e9f;
                for (int r : picked) {
                    float x0, y0, x1, y1;
                    box(runs[r], x0, y0, x1, y1);
                    m.bx0 = std::min(m.bx0, x0); m.by0 = std::min(m.by0, y0); m.bx1 = std::max(m.bx1, x1); m.by1 = std::max(m.by1, y1);
                }
            }
            m.found = true;
        }
    }

    void build_paths() {
        for (auto& r : runs) {
            if (r.path || r.mode == 5) continue;
            CGMutablePathRef path = CGPathCreateMutable();
            const float* q = r.points.data();
            for (uint8_t kind : r.kinds) {
                switch (kind) {
                case 0: CGPathMoveToPoint(path, nullptr, q[0], q[1]); q += 2; break;
                case 1: CGPathAddLineToPoint(path, nullptr, q[0], q[1]); q += 2; break;
                case 2: CGPathAddCurveToPoint(path, nullptr, q[0], q[1], q[2], q[3], q[4], q[5]); q += 6; break;
                case 3: CGPathCloseSubpath(path); break;
                }
            }
            r.path = path;
            r.bounds = CGRectInset(CGPathGetBoundingBox(path), -r.width - 1, -r.width - 1);
        }
    }

    // One run in the current transform. Clips nest as saved graphics states.
    static void draw_run(CGContextRef c, const Run& run, int& depth) {
        if (run.mode == 4) {
            CGContextSaveGState(c);
            depth++;
            if (run.path && !CGPathIsEmpty(run.path)) { CGContextAddPath(c, run.path); CGContextClip(c); }
            return;
        }
        if (run.mode == 5) {
            if (depth) { CGContextRestoreGState(c); depth--; }
            return;
        }
        if (!run.path || CGPathIsEmpty(run.path)) return;
        CGContextAddPath(c, run.path);
        if (run.mode == 3) {
            set_stroke(c, run.argb);
            CGContextSetLineWidth(c, run.width);
            CGContextStrokePath(c);
        } else if (run.mode == 2) {
            set_fill(c, run.argb);
            CGContextEOFillPath(c);
        } else {
            // A gradient band is drawn without antialiasing: the bands then tile without seams.
            set_fill(c, run.argb);
            CGContextSetShouldAntialias(c, false);
            CGContextFillPath(c);
            CGContextSetShouldAntialias(c, true);
        }
    }

    float art_scale() const { return std::min(width / (art_w * (1 + panel_c_w / panel_frame_w)), height / art_h); }
    float sx() const { return art_scale() * art_w / panel_frame_w; }
    float left() const { return roundf(panel_c_w * sx()); }
    float aspect() const { return art_w * (1 + panel_c_w / panel_frame_w) / art_h; }

    // The static art: drawn once per size into a layer at the screen's resolution.
    void render_art(CGContextRef view) {
        if (art_layer) { CGLayerRelease(art_layer); art_layer = nullptr; }
        if (width < 8 || height < 8) return;
        art_layer = CGLayerCreateWithContext(view, CGSizeMake(width, height), nullptr);
        CGContextRef c = CGLayerGetContext(art_layer);
        CGContextTranslateCTM(c, 0, height);  // y down, as the art
        CGContextScaleCTM(c, 1, -1);
        set_fill(c, panel_blue);
        CGContextFillRect(c, CGRectMake(0, 0, width, height));
        CGContextSaveGState(c);
        CGContextTranslateCTM(c, left(), 0);
        float scale = art_scale();
        CGContextScaleCTM(c, scale, scale);
        int depth = 0;
        for (auto& r : runs) if (!r.moving) draw_run(c, r, depth);
        while (depth--) CGContextRestoreGState(c);
        CGContextRestoreGState(c);
        draw_panel_c(c);
    }

    void draw_panel_c(CGContextRef c) {
        float s = sx();
        CGContextSaveGState(c);
        CGContextTranslateCTM(c, left(), 0);
        const float x0 = -226, x1 = -12, line = 2.7f * s;
        struct Box { float y0, y1, bar; bool title_top; NSString* title; };
        const Box boxes[] = {{20, 230, 38, true, @"WHEEL MODE"}, {250, 440, 422, false, @"KEYBOARD SENSE"}, {460, 667, 649, false, @"RIBBON"}};
        NSFont* title = [NSFont fontWithName:@"Arial-BoldMT" size:13.5f * s] ?: [NSFont boldSystemFontOfSize:13.5f * s];
        NSFont* label = [NSFont fontWithName:@"Arial-BoldMT" size:10.5f * s] ?: [NSFont boldSystemFontOfSize:10.5f * s];
        NSColor* cream = [NSColor colorWithSRGBRed:0xec / 255.0 green:0xe5 / 255.0 blue:0xc6 / 255.0 alpha:1];
        NSGraphicsContext* ns = [NSGraphicsContext graphicsContextWithCGContext:c flipped:YES];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:ns];
        auto text = [&](NSFont* font, NSString* str, float cx, float cy) {
            NSDictionary* attrs = @{NSFontAttributeName: font, NSForegroundColorAttributeName: cream};
            NSSize size = [str sizeWithAttributes:attrs];
            [str drawAtPoint:NSMakePoint(cx * s - size.width / 2, cy * s - size.height / 2) withAttributes:attrs];
        };
        set_stroke(c, panel_cream);
        CGContextSetLineWidth(c, line);
        for (const Box& b : boxes) {
            CGPathRef rounded = CGPathCreateWithRoundedRect(CGRectMake(x0 * s, b.y0 * s, (x1 - x0) * s, (b.y1 - b.y0) * s), 15 * s, 15 * s, nullptr);
            CGContextAddPath(c, rounded);
            CGContextStrokePath(c);
            CGPathRelease(rounded);
            CGContextMoveToPoint(c, x0 * s, b.bar * s);
            CGContextAddLineToPoint(c, x1 * s, b.bar * s);
            CGContextStrokePath(c);
            text(title, b.title, (x0 + x1) / 2, b.title_top ? (b.y0 + b.bar) / 2 : (b.bar + b.y1) / 2);
        }
        text(label, @"STND", -195, 82);
        text(label, @"M.MORPH", -145, 82);
        text(label, @"AFTERTOUCH", -68, 96);
        text(label, @"EXCHANGE", -68, 111);
        text(label, @"VELOCITY", -170, 372);
        text(label, @"AFTERTOUCH", -68, 372);
        text(label, @"RELATIVE", -170, 592);
        text(label, @"HOLD", -68, 592);
        [NSGraphicsContext restoreGraphicsState];
        for (const PanelLed& led : panel_leds) {
            if (led.x >= 0 || led.lamp) continue;
            set_fill(c, 0xff0c0c0e);
            CGContextFillEllipseInRect(c, CGRectMake((led.x - 3.4f) * s, (led.y - 3.4f) * s, 6.8f * s, 6.8f * s));
            set_stroke(c, panel_cream);
            CGContextSetLineWidth(c, 0.9f * s);
            CGContextStrokeEllipseInRect(c, CGRectMake((led.x - 3.6f) * s, (led.y - 3.6f) * s, 7.2f * s, 7.2f * s));
        }
        for (const PanelKey& k : panel_keys) {
            if (k.x >= 0) continue;
            set_fill(c, 0xff141416);
            CGContextFillEllipseInRect(c, CGRectMake((k.x - 21) * s, (k.y - 12) * s, 42 * s, 24 * s));
            set_fill(c, 0xff3a3a3e);
            CGContextFillEllipseInRect(c, CGRectMake((k.x - 3 - 13) * s, (k.y - 4 - 5) * s, 26 * s, 10 * s));
        }
        CGContextRestoreGState(c);
    }

    // seen: the part in view, in units after the left shift; a control outside it is not drawn.
    void draw_moving(CGContextRef c, CGRect seen = CGRectInfinite) {
        int count = sizeof panel_pots / sizeof panel_pots[0];
        float scale = art_scale();
        const float f = sx();
        auto in_view = [&](const Moving& m, bool slider) {
            if (CGRectIsInfinite(seen)) return true;
            CGRect box = slider ? CGRectMake(m.bx0, m.by0 - m.travel, m.bx1 - m.bx0, m.by1 - m.by0 + m.travel)
                                : CGRectMake(m.cx - 2 * m.radius - 2, m.cy - 2 * m.radius - 2, 4 * m.radius + 4, 4 * m.radius + 4);
            if (!slider && m.radius <= 0) box = CGRectMake(m.cx - 60, m.cy - 60, 120, 120);
            box = CGRectMake(box.origin.x * scale, box.origin.y * scale, box.size.width * scale, box.size.height * scale);
            return (bool)CGRectIntersectsRect(box, seen);
        };
        for (int i = 0; i < count; i++) {
            const PanelPot& p = panel_pots[i];
            if (!p.slider || !moving[i].found) continue;
            if (!CGRectIsInfinite(seen) && !CGRectIntersectsRect(CGRectMake((p.x - 6) * f, (p.y - 46) * f, 12 * f, 130 * f), seen)) continue;
            float top = p.y - 44.6f, bottom = p.y + 81;
            set_fill(c, panel_blue);
            CGContextFillRect(c, CGRectMake((p.x - 5.2f) * f, top * f, 10.4f * f, (bottom + 0.9f - top) * f));
            set_fill(c, 0xff0c0c0e);
            CGContextFillRect(c, CGRectMake((p.x - 0.9f) * f, (p.y - 43.6f) * f, 1.7f * f, (bottom - 2.1f - (p.y - 43.6f)) * f));
        }
        auto draw_part = [&](const Moving& m, bool slider, float value, float angle_deg) {
            CGContextSaveGState(c);
            CGContextScaleCTM(c, scale, scale);
            if (slider) {
                CGContextTranslateCTM(c, 0, -value * m.travel);
            } else {
                CGContextTranslateCTM(c, m.cx, m.cy);
                CGContextRotateCTM(c, angle_deg * (float)M_PI / 180);
                CGContextTranslateCTM(c, -m.cx, -m.cy);
            }
            int depth = 0;
            for (int r : m.runs) draw_run(c, runs[r], depth);
            while (depth--) CGContextRestoreGState(c);
            CGContextRestoreGState(c);
        };
        for (int i = 0; i < count; i++) {
            const Moving& m = moving[i];
            if (!m.found || !in_view(m, panel_pots[i].slider)) continue;
            float value = pot_value[i] / 255.f;
            draw_part(m, panel_pots[i].slider, value, m.lo + value * (m.hi - m.lo) - m.rest);
        }
        if (dial.found && in_view(dial, false)) draw_part(dial, false, 0, dial_angle);
    }

    static void dot_cover(int pixels, int cells, std::vector<int>& first, std::vector<float>& cover) {
        const float pitch = (float)pixels / cells, dot = 0.86f;
        first.assign(pixels, 0);
        cover.assign(pixels * 3, 0.f);
        for (int p = 0; p < pixels; p++) {
            int c0 = std::min(cells - 1, (int)(p / pitch));
            first[p] = c0;
            for (int k = 0; k < 3 && c0 + k < cells; k++) {
                float a = (c0 + k) * pitch, b = a + dot * pitch;
                float lo = std::max(a, (float)p), hi = std::min(b, (float)p + 1);
                cover[p * 3 + k] = std::max(0.f, hi - lo);
            }
        }
    }

    // The LCD as an even dot matrix, computed at the exact device pixel size (backing: pixels a point).
    void draw_lcd(CGContextRef c, float backing) {
        float s = sx() * backing;
        int x0 = (int)lroundf(panel_lcd_x0 * s), y0 = (int)lroundf(panel_lcd_y0 * s);
        int w = (int)lroundf(panel_lcd_x1 * s) - x0, h = (int)lroundf(panel_lcd_y1 * s) - y0;
        if (w < 8 || h < 4) return;
        if (!lcd_have) return;
        bool changed = lcd_pending || !lcd_valid || !lcd_image || w != lcd_w || h != lcd_h;
        lcd_pending = false;
        if (changed) {
            std::vector<int> fx, fy;
            std::vector<float> cx, cy;
            dot_cover(w, 240, fx, cx);
            dot_cover(h, 64, fy, cy);
            const float gap[3] = {0xea, 0xec, 0xfa}, off[3] = {0xdc, 0xdf, 0xf4}, on[3] = {0x48, 0x46, 0xc0};
            std::vector<uint8_t> pixels((size_t)w * h * 4);
            for (int py = 0; py < h; py++) for (int px = 0; px < w; px++) {
                float lit = 0, unlit = 0;
                for (int j = 0; j < 3; j++) {
                    float wy = cy[py * 3 + j];
                    if (wy <= 0) continue;
                    int row = fy[py] + j;
                    for (int i = 0; i < 3; i++) {
                        float wx = cx[px * 3 + i];
                        if (wx <= 0) continue;
                        int col = fx[px] + i;
                        bool background = lcd_shown[row * 30 + (col >> 3)] & (0x80 >> (col & 7));
                        (background ? unlit : lit) += wx * wy;
                    }
                }
                float rest = std::max(0.f, 1 - lit - unlit);
                uint8_t* out = &pixels[((size_t)py * w + px) * 4];
                for (int k = 0; k < 3; k++) out[k] = (uint8_t)std::min(255.f, gap[k] * rest + off[k] * unlit + on[k] * lit + 0.5f);
                out[3] = 255;
            }
            if (lcd_image) CGImageRelease(lcd_image);
            CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            CFDataRef bytes = CFDataCreate(nullptr, pixels.data(), (CFIndex)pixels.size());
            CGDataProviderRef provider = CGDataProviderCreateWithCFData(bytes);
            lcd_image = CGImageCreate(w, h, 8, 32, w * 4, space, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, provider, nullptr, false, kCGRenderingIntentDefault);
            CGDataProviderRelease(provider);
            CFRelease(bytes);
            CGColorSpaceRelease(space);
            lcd_w = w; lcd_h = h;
            lcd_valid = true;
        }
        if (!lcd_image) return;
        CGContextSetInterpolationQuality(c, kCGInterpolationNone);
        draw_upright_image(c, lcd_image, CGRectMake(x0 / backing, y0 / backing, w / backing, h / backing));
        CGContextSetInterpolationQuality(c, kCGInterpolationDefault);
    }

    // The area a control covers in view units, with its marks and shadow: what to redraw when it moves.
    CGRect dial_rect() const {
        float s = sx(), r = (panel_dial_r + 24) * s;
        return CGRectMake(left() + panel_dial_x * s - r, panel_dial_y * s - r, 2 * r, 2 * r);
    }
    CGRect pot_rect(int i) const {
        const PanelPot& p = panel_pots[i];
        float s = sx();
        CGRect r = p.slider ? CGRectMake(p.x - 26, p.y - 66, 52, 160) : CGRectMake(p.x - 48, p.y - 48, 96, 96);
        return CGRectMake(left() + r.origin.x * s, r.origin.y * s, r.size.width * s, r.size.height * s);
    }
    CGRect drag_rect() const {
        if (drag_pot >= 0) return pot_rect(drag_pot);
        if (drag_dial) return dial_rect();
        return CGRectNull;
    }

    CGRect lcd_rect() const {
        float s = sx();
        return CGRectInset(CGRectMake(left() + panel_lcd_x0 * s, panel_lcd_y0 * s, (panel_lcd_x1 - panel_lcd_x0) * s, (panel_lcd_y1 - panel_lcd_y0) * s), -2, -2);
    }

    void paint(CGContextRef c, float backing, CGRect dirty) {
        if (!art_layer || art_backing != backing) { render_art(c); art_backing = backing; }
        set_fill(c, panel_blue);
        CGContextFillRect(c, CGRectMake(0, 0, width, height));
        if (art_layer) {
            CGContextSaveGState(c);
            CGContextTranslateCTM(c, 0, height);
            CGContextScaleCTM(c, 1, -1);
            CGContextDrawLayerAtPoint(c, CGPointZero, art_layer);
            CGContextRestoreGState(c);
        }
        draw_dynamic(c, backing, false, dirty);
    }

    // The moving parts, LCD, lamps and marks, over the static art. visible: view units; with
    // vector_lcd the LCD is drawn as dots (sharp at any magnification) instead of the cached bitmap.
    void draw_dynamic(CGContextRef c, float backing, bool vector_lcd, CGRect visible) {
        float s = sx();
        CGContextSaveGState(c);
        CGContextTranslateCTM(c, left(), 0);
        if (vector_lcd) draw_lcd_vector(c, CGRectOffset(visible, -left(), 0));
        else draw_lcd(c, backing);
        draw_moving(c, CGRectOffset(CGRectInset(visible, -12, -12), -left(), 0));
        uint8_t latches[16];
        memcpy(latches, leds_shown, sizeof latches);
        for (const PanelLed& led : panel_leds) {
            if (!(latches[led.latch] >> led.bit & 1)) continue;
            float x = led.x * s, y = led.y * s;
            float r = led.lamp ? 5.6f : 3.1f;
            auto ellipse = [&](float cx, float cy, float rx, float ry) { CGContextFillEllipseInRect(c, CGRectMake(cx - rx, cy - ry, rx * 2, ry * 2)); };
            CGContextSetRGBFillColor(c, 1.f, 0.15f, 0.08f, 0.28f);
            ellipse(x, y, r * 1.6f * s, r * 1.6f * s);
            CGContextSetRGBFillColor(c, 1.f, 0.16f, 0.1f, 1.f);
            ellipse(x, y, r * s, r * s);
            CGContextSetRGBFillColor(c, 1.f, 0.75f, 0.6f, 0.85f);
            ellipse(x - r * 0.29f * s, y - r * 0.32f * s, r * 0.32f * s, r * 0.25f * s);
        }
        CGContextSetRGBStrokeColor(c, 0.85f, 0.1f, 0.1f, 1);
        int count = sizeof panel_pots / sizeof panel_pots[0];
        for (int i = 0; i < count; i++) {
            const PanelPot& p = panel_pots[i];
            if (moving[i].found) continue;
            float value = pot_value[i] / 255.f;
            if (p.slider) {
                float y = (p.y + 44 - value * 88) * s;
                CGContextSetLineWidth(c, std::max(1.5f, 4 * s));
                CGContextMoveToPoint(c, (p.x - 12) * s, y);
                CGContextAddLineToPoint(c, (p.x + 12) * s, y);
            } else {
                float angle = (-135 + value * 270) * 3.14159265f / 180, rr = 22 * s;
                CGContextSetLineWidth(c, std::max(1.5f, 3.5f * s));
                CGContextMoveToPoint(c, p.x * s, p.y * s);
                CGContextAddLineToPoint(c, p.x * s + rr * sinf(angle), p.y * s - rr * cosf(angle));
            }
            CGContextStrokePath(c);
        }
        if (down_key >= 0) {
            const PanelKey& k = panel_keys[down_key];
            CGContextSetRGBFillColor(c, 0.85f, 0.1f, 0.1f, 0.45f);
            CGContextFillEllipseInRect(c, CGRectMake((k.x - 21) * s, (k.y - 12) * s, 42 * s, 24 * s));
        }
        CGContextRestoreGState(c);
    }

    // The static art drawn as vectors, only the runs that reach `visible` (view units): for the
    // magnifier, sharp at any zoom where the cached layer would be blurred.
    void draw_static_vector(CGContextRef c, CGRect visible) {
        set_fill(c, panel_blue);
        CGContextFillRect(c, visible);
        float scale = art_scale();
        if (scale <= 0) return;
        CGRect art_visible = CGRectMake((visible.origin.x - left()) / scale, visible.origin.y / scale,
                                        visible.size.width / scale, visible.size.height / scale);
        CGContextSaveGState(c);
        CGContextTranslateCTM(c, left(), 0);
        CGContextScaleCTM(c, scale, scale);
        int depth = 0;
        for (auto& r : runs) {
            if (r.moving) continue;
            if (r.mode < 4 && !CGRectIntersectsRect(r.bounds, art_visible)) continue;
            draw_run(c, r, depth);
        }
        while (depth--) CGContextRestoreGState(c);
        CGContextRestoreGState(c);
        if (visible.origin.x < left()) draw_panel_c(c);  // its text is the costly part: only when seen
    }

    // The LCD as one rectangle a dot, for the cells inside `visible` (units after the left shift).
    void draw_lcd_vector(CGContextRef c, CGRect visible) {
        // Not lcd_valid: a change of screen resolution clears it while the bitmap is still shown,
        // and the magnifier then showed the art's empty LCD glass, black, until the text changed.
        if (!lcd_have) return;
        float s = sx();
        float x0 = panel_lcd_x0 * s, y0 = panel_lcd_y0 * s;
        float w = (panel_lcd_x1 - panel_lcd_x0) * s, h = (panel_lcd_y1 - panel_lcd_y0) * s;
        CGRect lcd = CGRectMake(x0, y0, w, h);
        CGRect part = CGRectIntersection(lcd, visible);
        if (CGRectIsNull(part) || CGRectIsEmpty(part)) return;
        const float px = w / 240, py = h / 64, dot = 0.86f;
        CGContextSetRGBFillColor(c, 0xea / 255.f, 0xec / 255.f, 0xfa / 255.f, 1);  // the gap colour
        CGContextFillRect(c, part);
        int c0 = std::max(0, (int)((part.origin.x - x0) / px) - 1), c1 = std::min(239, (int)((CGRectGetMaxX(part) - x0) / px) + 1);
        int r0 = std::max(0, (int)((part.origin.y - y0) / py) - 1), r1 = std::min(63, (int)((CGRectGetMaxY(part) - y0) / py) + 1);
        for (int pass = 0; pass < 2; pass++) {  // unlit dots, then lit ones: two fills, not one a dot
            for (int row = r0; row <= r1; row++)
                for (int col = c0; col <= c1; col++) {
                    bool background = lcd_shown[row * 30 + (col >> 3)] & (0x80 >> (col & 7));
                    if (background != (pass == 0)) continue;
                    CGContextAddRect(c, CGRectMake(x0 + col * px, y0 + row * py, px * dot, py * dot));
                }
            if (pass == 0) CGContextSetRGBFillColor(c, 0xdc / 255.f, 0xdf / 255.f, 0xf4 / 255.f, 1);
            else CGContextSetRGBFillColor(c, 0x48 / 255.f, 0x46 / 255.f, 0xc0 / 255.f, 1);
            CGContextFillPath(c);
        }
    }

    // The magnifier's static art: the whole panel drawn once from the vectors at the magnifier's
    // zoom, then only copied. Redrawing the art's many layers of gradient bands at every frame
    // stalled the magnifier. About 40 MB at 2x on a Retina screen; made again when the zoom, the
    // panel size or the screen changes, and freed when the magnifier is turned off.
    CGLayerRef zoom_layer = nullptr;
    float zoom_layer_z = 0, zoom_layer_bk = 0;
    int zoom_layer_w = 0, zoom_layer_h = 0;

    void forget_zoom_layer() {
        if (zoom_layer) CGLayerRelease(zoom_layer);
        zoom_layer = nullptr;
    }

    CGLayerRef zoomed_art(CGContextRef view, float z, float bk) {
        if (zoom_layer && z == zoom_layer_z && bk == zoom_layer_bk && width == zoom_layer_w && height == zoom_layer_h) return zoom_layer;
        forget_zoom_layer();
        if (width < 8 || height < 8) return nullptr;
        zoom_layer = CGLayerCreateWithContext(view, CGSizeMake(width * z, height * z), nullptr);
        if (!zoom_layer) return nullptr;
        CGContextRef c = CGLayerGetContext(zoom_layer);
        CGContextTranslateCTM(c, 0, height * z);  // y down, as the panel
        CGContextScaleCTM(c, 1, -1);
        CGContextScaleCTM(c, z, z);
        draw_static_vector(c, CGRectMake(0, 0, width, height));
        zoom_layer_z = z; zoom_layer_bk = bk; zoom_layer_w = width; zoom_layer_h = height;
        return zoom_layer;
    }

    // Where the control being turned is now, in view units: the magnifier follows it while it moves.
    bool drag_center(float& x, float& y) const {
        float s = sx();
        if (drag_pot >= 0) {
            const PanelPot& p = panel_pots[drag_pot];
            float value = pot_value[drag_pot] / 255.f;
            x = left() + p.x * s;
            y = p.y * s;
            if (p.slider && moving[drag_pot].found) y = ((moving[drag_pot].by0 + moving[drag_pot].by1) / 2 - value * moving[drag_pot].travel) * art_scale();
            else if (p.slider) y = (p.y + 44 - value * 88) * s;
            return true;
        }
        if (drag_dial) { x = left() + panel_dial_x * s; y = panel_dial_y * s; return true; }
        return false;
    }

    void set_pot(int i, int value) {
        value = std::max(0, std::min(255, value));
        pot_value[i] = value;
        engine.set_pot(panel_pots[i].channel, panel_pots[i].mux, value);
    }

    void mouse_down(float x, float y) {
        float s = sx();
        float fx = (x - left()) / s, fy = y / s;
        int count = sizeof panel_pots / sizeof panel_pots[0];
        for (int i = 0; i < count; i++) {
            const PanelPot& p = panel_pots[i];
            bool hit = p.slider ? fabsf(fx - p.x) < 20 && fabsf(fy - p.y) < 62 : hypotf(fx - p.x, fy - p.y) < 30;
            if (hit) { drag_pot = i; drag_start_y = (int)y; drag_start_value = pot_value[i]; return; }
        }
        int keys = sizeof panel_keys / sizeof panel_keys[0];
        for (int i = 0; i < keys; i++) {
            const PanelKey& k = panel_keys[i];
            if (fabsf(fx - k.x) < 24 && fabsf(fy - k.y) < 15) {
                down_key = i;
                engine.key(k.bar, k.bit, true);
                return;
            }
        }
        if (hypotf(fx - panel_dial_x, fy - panel_dial_y) < panel_dial_r) {
            drag_dial = true;
            dial_grab = atan2f(fx - panel_dial_x, -(fy - panel_dial_y)) * 180 / 3.14159265f;
            dial_rest = 0;
        }
    }

    void mouse_move(float x, float y) {
        if (drag_pot >= 0) set_pot(drag_pot, drag_start_value + (int)((drag_start_y - y) * 256 / std::max(60, height / 3)));
        if (drag_dial) {
            float s = sx();
            float a = atan2f((x - left()) / s - panel_dial_x, -(y / s - panel_dial_y)) * 180 / 3.14159265f;
            float d = a - dial_grab;
            if (d > 180) d -= 360;
            if (d < -180) d += 360;
            dial_grab = a;
            dial_angle += d;
            dial_rest += d;
            int detents = (int)(dial_rest / 15);
            if (detents) { engine.turn(detents); dial_rest -= detents * 15.f; }
        }
    }

    void mouse_up() {
        if (down_key >= 0) engine.key(panel_keys[down_key].bar, panel_keys[down_key].bit, false);
        down_key = drag_pot = -1;
        drag_dial = false;
    }
};

int PanelView::output_gain_db() {
    int db = gain_db.load();
    if (db < 0) { db = read_gain(); gain_db = db; }
    return db;
}
float PanelView::output_gain() { return powf(10.f, output_gain_db() / 20.f); }

// ---------------------------------------------------------------- the LCD in a window of its own

// The 240 x 64 LCD drawn as dots, as large as its window, the unit's backlight and dot colours.
@interface OB12_LCD_CLASS : NSView
- (instancetype)initWithMachine:(ob12mac::MachineRef)machine;
@property double contentAspect;  // the window's content, width over height: snapped back to it after a resize
@end

// In Logic the editor runs in a hosting process where a view's drawing is only recorded, and
// Core Animation replays it later on the GPU, on one serial queue. The panel's thousands of
// curves piled up there faster than they were replayed when the dial was turned fast, and the
// editor showed an image seconds, then minutes, old: frozen until reopened. So the views draw on
// the main thread into a bitmap of their own, and hand the system only the changed piece of it,
// a single image: cheap to replay, and a slow frame only delays the next one.
struct Canvas {
    CGContextRef bitmap = nullptr;
    size_t pw = 0, ph = 0;
    ~Canvas() { if (bitmap) CGContextRelease(bitmap); }

    void paint(NSView* view, NSRect dirty, void (^draw)(CGRect dirty)) {
        CGContextRef out = [NSGraphicsContext currentContext].CGContext;
        NSRect b = view.bounds;
        if (!out || b.size.width < 1 || b.size.height < 1) return;
        float bk = view.window ? (float)view.window.backingScaleFactor : 2.f;
        size_t w = (size_t)ceil(b.size.width * bk), h = (size_t)ceil(b.size.height * bk);
        if (!bitmap || w != pw || h != ph) {
            if (bitmap) CGContextRelease(bitmap);
            CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            bitmap = CGBitmapContextCreate(nullptr, w, h, 8, 0, space, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
            CGColorSpaceRelease(space);
            if (!bitmap) return;
            pw = w; ph = h;
            dirty = b;  // a new bitmap: all of it
        }
        // The dirty part on whole pixels: drawn, then copied, exactly.
        long x0 = std::max(0L, (long)floor(NSMinX(dirty) * bk)), y0 = std::max(0L, (long)floor(NSMinY(dirty) * bk));
        long x1 = std::min((long)w, (long)ceil(NSMaxX(dirty) * bk)), y1 = std::min((long)h, (long)ceil(NSMaxY(dirty) * bk));
        if (x1 <= x0 || y1 <= y0) return;
        CGRect part = CGRectMake(x0 / bk, y0 / bk, (x1 - x0) / bk, (y1 - y0) / bk);

        CGContextSaveGState(bitmap);
        CGContextScaleCTM(bitmap, bk, bk);
        CGContextTranslateCTM(bitmap, 0, b.size.height);  // flipped, as the views: row 0 on top
        CGContextScaleCTM(bitmap, 1, -1);
        CGContextClipToRect(bitmap, part);
        [NSGraphicsContext saveGraphicsState];
        NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithCGContext:bitmap flipped:YES];
        draw(part);
        [NSGraphicsContext restoreGraphicsState];
        CGContextRestoreGState(bitmap);

        size_t cw = (size_t)(x1 - x0), ch = (size_t)(y1 - y0), stride = CGBitmapContextGetBytesPerRow(bitmap);
        const uint8_t* src = (const uint8_t*)CGBitmapContextGetData(bitmap);
        CFMutableDataRef bytes = CFDataCreateMutable(nullptr, (CFIndex)(cw * ch * 4));
        CFDataSetLength(bytes, (CFIndex)(cw * ch * 4));
        uint8_t* dst = CFDataGetMutableBytePtr(bytes);
        for (size_t row = 0; row < ch; row++) memcpy(dst + row * cw * 4, src + (y0 + row) * stride + x0 * 4, cw * 4);
        CGDataProviderRef provider = CGDataProviderCreateWithCFData(bytes);
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGImageRef piece = CGImageCreate(cw, ch, 8, 32, cw * 4, space, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little, provider, nullptr, false, kCGRenderingIntentDefault);
        CGColorSpaceRelease(space);
        CGDataProviderRelease(provider);
        CFRelease(bytes);
        if (!piece) return;
        CGContextSaveGState(out);
        CGContextSetInterpolationQuality(out, kCGInterpolationNone);
        CGContextTranslateCTM(out, 0, CGRectGetMaxY(part));  // the view is flipped, the image is not
        CGContextScaleCTM(out, 1, -1);
        CGContextDrawImage(out, CGRectMake(part.origin.x, 0, part.size.width, part.size.height), piece);
        CGContextRestoreGState(out);
        CGImageRelease(piece);
    }
};

@implementation OB12_LCD_CLASS {
    Canvas _canvas;
    ob12mac::MachineRef _machine;
    uint8_t _bits[30 * 64];
    bool _valid;
    NSTimer* _timer;
    NSSize _before;  // the size when the live resize began
}

- (instancetype)initWithMachine:(ob12mac::MachineRef)machine {
    self = [super initWithFrame:NSMakeRect(0, 0, 100, 30)];
    if (self) {
        _machine = machine;
        memset(_bits, 0, sizeof _bits);
        _valid = false;
        __weak OB12_LCD_CLASS* weak = self;
        _timer = [NSTimer timerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer*) { [weak poll]; }];
        [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
    }
    return self;
}

- (void)dealloc { [_timer invalidate]; }
- (void)stop { [_timer invalidate]; _timer = nil; }

// Redrawn only when the LCD changes: drawing its 15360 dots 30 times a second for nothing took
// time from the panel and its magnifier.
- (void)poll {
    uint8_t now[30 * 64];
    if (!_machine.engine || !_machine.engine->lcd(now)) return;
    if (_valid && !memcmp(now, _bits, sizeof now)) return;
    memcpy(_bits, now, sizeof now);
    _valid = true;
    [self setNeedsDisplay:YES];
}

// macOS does not always apply a window's aspect constraint (a plug-in in Logic's hosting process),
// so the window is brought back to the LCD's proportions when the user lets go: the side pulled
// most leads, the top edge stays put. While pulling, drawRect keeps the LCD itself undistorted.
- (void)viewWillStartLiveResize { [super viewWillStartLiveResize]; _before = self.bounds.size; }
- (void)viewDidEndLiveResize {
    [super viewDidEndLiveResize];
    [self snapToAspect];
}
- (void)snapToAspect {
    NSWindow* win = self.window;
    double a = self.contentAspect;
    if (!win || a <= 0) return;
    NSRect inner = [win contentRectForFrameRect:win.frame];
    if (fabs(inner.size.width / inner.size.height - a) < 0.005) return;
    double dw = fabs(inner.size.width - _before.width), dh = fabs(inner.size.height - _before.height) * a;
    double top = NSMaxY(inner);
    if (dw >= dh) inner.size.height = inner.size.width / a;
    else inner.size.width = inner.size.height * a;
    inner.origin.y = top - inner.size.height;
    [win setFrame:[win frameRectForContentRect:inner] display:YES animate:NO];
}
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)dirty {
    __weak __typeof(self) me = self;
    _canvas.paint(self, dirty, ^(CGRect part) { [me paintDirty:part]; });
}
- (BOOL)isOpaque { return YES; }

- (void)paintDirty:(NSRect)dirty {
    CGContextRef c = [NSGraphicsContext currentContext].CGContext;
    NSRect b = self.bounds;
    CGContextSetRGBFillColor(c, 0.05f, 0.05f, 0.06f, 1);  // the bezel
    CGContextFillRect(c, b);
    // The LCD at its own proportions, centred: never stretched, whatever the window's shape.
    const float lcd_aspect = (panel_lcd_x1 - panel_lcd_x0) / (panel_lcd_y1 - panel_lcd_y0);
    float m = (float)std::min(b.size.width, b.size.height) * 0.07f;
    CGRect room = CGRectInset(b, m, m);
    float w = (float)room.size.width, h = (float)room.size.height;
    if (w / h > lcd_aspect) w = h * lcd_aspect; else h = w / lcd_aspect;
    CGRect lcd = CGRectMake(room.origin.x + (room.size.width - w) / 2, room.origin.y + (room.size.height - h) / 2, w, h);
    CGContextSetRGBFillColor(c, 0xea / 255.f, 0xec / 255.f, 0xfa / 255.f, 1);  // the gap between dots
    CGContextFillRect(c, lcd);
    if (!_valid) return;
    const float px = (float)lcd.size.width / 240, py = (float)lcd.size.height / 64, dot = 0.86f;
    for (int pass = 0; pass < 2; pass++) {  // unlit dots, then lit ones
        for (int row = 0; row < 64; row++)
            for (int col = 0; col < 240; col++) {
                bool background = _bits[row * 30 + (col >> 3)] & (0x80 >> (col & 7));
                if (background != (pass == 0)) continue;
                CGContextAddRect(c, CGRectMake(lcd.origin.x + col * px, lcd.origin.y + row * py, px * dot, py * dot));
            }
        if (pass == 0) CGContextSetRGBFillColor(c, 0xdc / 255.f, 0xdf / 255.f, 0xf4 / 255.f, 1);
        else CGContextSetRGBFillColor(c, 0x48 / 255.f, 0x46 / 255.f, 0xc0 / 255.f, 1);
        CGContextFillPath(c);
    }
}

@end

// ---------------------------------------------------------------- the NSView

@interface OB12_PANEL_CLASS : NSView <NSWindowDelegate>
- (instancetype)initWithMachine:(ob12mac::MachineRef)machine;
@end

@implementation OB12_PANEL_CLASS {
    Canvas _canvas;
    ob12mac::MachineRef _machine;
    std::unique_ptr<PanelArt> _art;
    NSTimer* _timer;
    LoupeSettings _loupe;
    NSPoint _mouse;          // the pointer, view units
    BOOL _mouseInside;
    NSButton* _loupeButton;
    NSTrackingArea* _tracking;
    NSButton* _lcdButton;
    NSSegmentedControl* _zoomPick;  // the magnifier's zoom and size, in the strip: no menu needed
    NSSegmentedControl* _sizePick;
    NSPanel* _lcdWindow;
    BOOL _lcdWanted;      // the user's choice, kept: the window comes back with the editor
    BOOL _closingForGood; // the editor goes away: closing the window is not the user turning it off
    id _activationObserver;
    BOOL _dirty;          // something changed: the next frame redraws the whole panel
    long _tick;
    CGRect _lastLoupe;    // where the magnifier is drawn, with its shadow (empty: nowhere)
    CGPoint _loupeAt;     // its centre, decided by the clock
    CGRect _partDirty;    // controls that moved since the last frame (null: none)
    float _ppm;           // points per millimetre of the screen below, kept
    NSScreen* _ppmScreen;
    pid_t _hostPid;       // the app that was in front when the editor opened: the host (Logic)
    double _lcdAspect;    // the LCD window's content, width over height
}

- (instancetype)initWithMachine:(ob12mac::MachineRef)machine {
    std::unique_ptr<PanelArt> art(new PanelArt(*machine.engine));
    art->load(ob12mac::rom_folder() + "/panel.ob12p");
    art->find_moving();
    art->find_dial();
    art->recolour();  // after the finders: they know the parts by the art's own colours
    art->build_paths();
    int w = load_width();
    int h = (int)lroundf(w / art->aspect());
    self = [super initWithFrame:NSMakeRect(0, 0, w, h + strip_height)];
    if (self) {
        _machine = machine;
        _art = std::move(art);
        _art->width = w;
        _art->height = h;
        _loupe = load_loupe();
        NSImage* glass = [NSImage imageWithSystemSymbolName:@"magnifyingglass" accessibilityDescription:@"Magnifier"];
        _loupeButton = [NSButton buttonWithTitle:@"Magnifier" image:glass target:self action:@selector(toggleLoupe:)];
        [_loupeButton setButtonType:NSButtonTypePushOnPushOff];
        _loupeButton.bezelStyle = NSBezelStyleRounded;
        _loupeButton.controlSize = NSControlSizeSmall;
        _loupeButton.font = [NSFont systemFontOfSize:[NSFont systemFontSizeForControlSize:NSControlSizeSmall]];
        _loupeButton.imagePosition = NSImageLeading;
        _loupeButton.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        _loupeButton.toolTip = @"Magnifier: hover over the panel to enlarge it. Right click for zoom and size.";
        _loupeButton.state = _loupe.on ? NSControlStateValueOn : NSControlStateValueOff;
        [_loupeButton sizeToFit];
        [self addSubview:_loupeButton];
        NSImage* screen = [NSImage imageWithSystemSymbolName:@"display" accessibilityDescription:@"LCD window"];
        _lcdButton = [NSButton buttonWithTitle:@"LCD ×2" image:screen target:self action:@selector(toggleLCD:)];
        [_lcdButton setButtonType:NSButtonTypePushOnPushOff];
        _lcdButton.bezelStyle = NSBezelStyleRounded;
        _lcdButton.controlSize = NSControlSizeSmall;
        _lcdButton.font = _loupeButton.font;
        _lcdButton.imagePosition = NSImageLeading;
        _lcdButton.appearance = _loupeButton.appearance;
        _lcdButton.toolTip = @"The LCD in a window of its own, twice its size on the panel. Resize the window to taste.";
        _lcdWanted = load_lcd_window();
        _lcdButton.state = _lcdWanted ? NSControlStateValueOn : NSControlStateValueOff;
        [_lcdButton sizeToFit];
        [self addSubview:_lcdButton];
        _zoomPick = [NSSegmentedControl segmentedControlWithLabels:@[@"×1.5", @"×2", @"×2.5"] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(zoomPicked:)];
        _sizePick = [NSSegmentedControl segmentedControlWithLabels:@[@"3 cm", @"3.5 cm", @"4 cm", @"5 cm"] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(sizePicked:)];
        for (NSSegmentedControl* pick in @[_zoomPick, _sizePick]) {
            pick.controlSize = NSControlSizeSmall;
            pick.font = _loupeButton.font;
            pick.appearance = _loupeButton.appearance;
            [self addSubview:pick];
        }
        _zoomPick.toolTip = @"Magnifier zoom";
        _sizePick.toolTip = @"Magnifier diameter on the screen";
        [self showLoupePicks];
        [self placeStrip];
        _partDirty = CGRectNull;
        __weak OB12_PANEL_CLASS* weak = self;
        _timer = [NSTimer timerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer*) { [weak clockTick]; }];
        [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
    }
    return self;
}

// The only place the whole panel is redrawn: at most 60 times a second while something moves,
// 30 otherwise (the LCD and the lamps). Mouse, wheel and dial events only mark the panel dirty;
// redrawing at their own rate (up to 120 a second) piled up and froze the editor.
- (void)clockTick {
    bool lcd = _art->poll_lcd();
    CGRect lamps = _art->poll_leds();
    bool leds = !CGRectIsNull(lamps);
    CGRect area = CGRectZero;
    if (_loupe.on) {
        NSPoint at = self.window ? [self convertPoint:self.window.mouseLocationOutsideOfEventStream fromView:nil] : _mouse;
        BOOL inside = self.window && self.window.isVisible && NSPointInRect(at, NSMakeRect(0, 0, _art->width, _art->height));
        float cx = (float)at.x, cy = (float)at.y;
        bool dragging = _art->drag_center(cx, cy);
        if (dragging || inside) {
            const float r = std::max(20.f, _loupe.mm * [self pointsPerMillimetre] / 2);
            area = CGRectMake(cx - r - 16, cy - r - 16, 2 * r + 32, 2 * r + 32);
            _loupeAt = CGPointMake(cx, cy);
        }
    }
    // Only what changed is redrawn: the whole panel when a control or a lamp did, the LCD when it
    // did, the magnifier where it was and where it goes. A full redraw at every pointer move kept
    // the main thread busy and Logic stalled.
    if (_dirty) [self setNeedsDisplay:YES];
    else {
        bool part = !CGRectIsNull(_partDirty) && !CGRectIsEmpty(_partDirty);
        if (part) [self setNeedsDisplayInRect:_partDirty];
        if (leds) { [self setNeedsDisplayInRect:lamps]; part = true; }
        if (lcd) [self setNeedsDisplayInRect:_art->lcd_rect()];
        if (!CGRectEqualToRect(area, _lastLoupe) || lcd || part) {
            if (!CGRectIsEmpty(_lastLoupe)) [self setNeedsDisplayInRect:_lastLoupe];
            if (!CGRectIsEmpty(area)) [self setNeedsDisplayInRect:area];
        }
    }
    _lastLoupe = area;
    _dirty = NO;
    _partDirty = CGRectNull;
}

- (void)dealloc {
    [_timer invalidate];
    [self forgetActivation];
    if (_lcdWindow) { _lcdWindow.delegate = nil; [(OB12_LCD_CLASS*)_lcdWindow.contentView stop]; [_lcdWindow close]; }
}

- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)dirty {
    __weak __typeof(self) me = self;
    _canvas.paint(self, dirty, ^(CGRect part) { [me paintDirty:part]; });
}
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    if (!_art) return;
    _art->width = (int)size.width;
    _art->height = std::max(8, (int)size.height - strip_height);
    _art->art_backing = 0;  // the art is drawn again at the new size
    [self placeStrip];
}

- (void)placeStrip {
    if (!_loupeButton || !_art) return;
    NSSize b = _loupeButton.frame.size;
    [_loupeButton setFrameOrigin:NSMakePoint(10, _art->height + (strip_height - b.height) / 2)];
    if (_lcdButton) {
        NSSize l = _lcdButton.frame.size;
        [_lcdButton setFrameOrigin:NSMakePoint(NSMaxX(_loupeButton.frame) + 8, _art->height + (strip_height - l.height) / 2)];
    }
    NSView* last = _lcdButton ?: _loupeButton;
    for (NSSegmentedControl* pick in @[_zoomPick ?: (id)NSNull.null, _sizePick ?: (id)NSNull.null]) {
        if (![pick isKindOfClass:NSSegmentedControl.class]) continue;
        [pick sizeToFit];
        NSSize p = pick.frame.size;
        [pick setFrameOrigin:NSMakePoint(NSMaxX(last.frame) + 12, _art->height + (strip_height - p.height) / 2)];
        last = pick;
    }
}

// ---- the LCD window

- (void)openLCD {
    if (_lcdWindow) return;
    float s = _art->sx();
    float w = std::max(240.f, (panel_lcd_x1 - panel_lcd_x0) * s * 2), h = w * (panel_lcd_y1 - panel_lcd_y0) / (panel_lcd_x1 - panel_lcd_x0);
    float m = std::min(w, h) * 0.07f / 0.86f;  // room for the bezel the view draws
    NSSize content = NSMakeSize(w + 2 * m, h + 2 * m);
    NSPanel* win = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, content.width, content.height)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable | NSWindowStyleMaskUtilityWindow
        backing:NSBackingStoreBuffered defer:NO];
    win.title = @"OB-12 LCD";
    win.floatingPanel = YES;
    // In front of everything while the host is the active app (Logic's plug-in windows float too,
    // so the plain floating level let a click on the editor bring the editor over the LCD), hidden
    // while another app is in front: see updateLCDVisibility.
    win.level = NSStatusWindowLevel;
    win.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorIgnoresCycle;
    win.hidesOnDeactivate = NO;
    win.releasedWhenClosed = NO;
    win.becomesKeyOnlyIfNeeded = YES;
    _lcdAspect = content.width / content.height;
    win.contentMinSize = NSMakeSize(content.width / 3, content.height / 3);
    OB12_LCD_CLASS* lcdView = [[OB12_LCD_CLASS alloc] initWithMachine:_machine];
    lcdView.contentAspect = content.width / content.height;
    win.contentView = lcdView;
    win.contentAspectRatio = content;  // where macOS honours it, the resize is constrained live
    win.delegate = self;
    if (![win setFrameUsingName:@"OB12 LCD"]) {  // the first time: just above the editor
        NSRect host = self.window ? self.window.frame : NSMakeRect(200, 200, 0, 0);
        NSRect frame = win.frame;
        frame.origin.x = NSMidX(host) - frame.size.width / 2;
        frame.origin.y = NSMaxY(host) + 12;
        NSRect visible = (self.window.screen ?: NSScreen.mainScreen).visibleFrame;
        if (NSMaxY(frame) > NSMaxY(visible)) frame.origin.y = NSMaxY(visible) - frame.size.height;
        [win setFrame:frame display:NO];
    }
    win.frameAutosaveName = @"OB12 LCD";
    {   // a frame saved earlier keeps its width and gets the LCD's proportions again
        NSRect inner = [win contentRectForFrameRect:win.frame];
        double h = inner.size.width / _lcdAspect;
        if (fabs(inner.size.height - h) > 1) {
            inner.origin.y += inner.size.height - h;  // the top edge stays where it was
            inner.size.height = h;
            [win setFrame:[win frameRectForContentRect:inner] display:NO];
        }
    }
    _lcdWindow = win;
    _lcdButton.state = NSControlStateValueOn;
    __weak OB12_PANEL_CLASS* weak = self;
    _activationObserver = [NSWorkspace.sharedWorkspace.notificationCenter addObserverForName:NSWorkspaceDidActivateApplicationNotification
        object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*) { [weak updateLCDVisibility]; }];
    [self updateLCDVisibility];
}

// Shown only while the host (Logic) is the active app; hidden while another app is in front.
- (void)updateLCDVisibility {
    if (!_lcdWindow) return;
    NSRunningApplication* front = NSWorkspace.sharedWorkspace.frontmostApplication;
    pid_t pid = front ? front.processIdentifier : 0;
    // Logic may run the plug-in in a hosting process of its own: the host is then not this process
    // but the app that was in front when the editor opened.
    BOOL host = !front || pid == getpid() || (_hostPid && pid == _hostPid) || !_hostPid;
    if (host) [_lcdWindow orderFrontRegardless];
    else [_lcdWindow orderOut:nil];
}

- (void)forgetActivation {
    if (_activationObserver) [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:_activationObserver];
    _activationObserver = nil;
}

- (void)closeLCDForGood:(BOOL)forGood {
    if (!_lcdWindow) return;
    _closingForGood = forGood;
    [(OB12_LCD_CLASS*)_lcdWindow.contentView stop];
    [_lcdWindow close];
    _closingForGood = NO;
}

// Resizing keeps the LCD's proportions: the side the user pulls most leads, the other follows.
- (NSSize)windowWillResize:(NSWindow*)sender toSize:(NSSize)frameSize {
    if (sender != _lcdWindow || _lcdAspect <= 0) return frameSize;
    NSSize now = [sender contentRectForFrameRect:sender.frame].size;
    NSRect want = [sender contentRectForFrameRect:NSMakeRect(0, 0, frameSize.width, frameSize.height)];
    if (fabs(want.size.width - now.width) >= fabs(want.size.height - now.height) * _lcdAspect)
        want.size.height = want.size.width / _lcdAspect;
    else
        want.size.width = want.size.height * _lcdAspect;
    NSSize least = sender.contentMinSize;
    if (want.size.width < least.width) { want.size.width = least.width; want.size.height = least.width / _lcdAspect; }
    return [sender frameRectForContentRect:want].size;
}

- (void)windowWillClose:(NSNotification*)note {
    if (note.object != _lcdWindow) return;
    [self forgetActivation];
    [(OB12_LCD_CLASS*)_lcdWindow.contentView stop];
    _lcdWindow.delegate = nil;
    NSPanel* closing = _lcdWindow;  // let it finish closing before the last reference goes
    dispatch_async(dispatch_get_main_queue(), ^{ (void)closing; });
    _lcdWindow = nil;
    if (!_closingForGood) {  // the user closed it
        _lcdWanted = NO;
        save_lcd_window(false);
        _lcdButton.state = NSControlStateValueOff;
    }
}

- (void)toggleLCD:(id)sender {
    if (_lcdWindow) {
        [self closeLCDForGood:NO];
    } else {
        _lcdWanted = YES;
        save_lcd_window(true);
        [self openLCD];
    }
}

// The window lives with the editor: it goes when the editor closes and comes back with it.
- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window) {  // the editor opens: the app in front now is the host
        NSRunningApplication* front = NSWorkspace.sharedWorkspace.frontmostApplication;
        if (front) _hostPid = front.processIdentifier;
    }
    if (!self.window) [self closeLCDForGood:YES];
    else if (_lcdWanted && !_lcdWindow) [self openLCD];
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc] initWithRect:NSZeroRect
        options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect
        owner:self userInfo:nil];
    [self addTrackingArea:_tracking];
}

// A move redraws only where the magnifier was and where it goes, not the whole panel: the pointer
// can send 120 moves a second, and a full redraw each time stalled the magnifier.
- (void)loupeMovedTo:(NSPoint)p {
    _mouse = p;
    // Nothing to do here: the clock reads the pointer at each frame and moves the magnifier.
}
- (void)mouseEntered:(NSEvent*)event { _mouseInside = YES; [self loupeMovedTo:[self at:event]]; }
- (void)mouseExited:(NSEvent*)event { _mouseInside = NO; }
- (void)mouseMoved:(NSEvent*)event { _mouseInside = YES; [self loupeMovedTo:[self at:event]]; }

// Points per millimetre on the screen that shows the panel, from the display's physical size.
- (float)pointsPerMillimetre {
    NSScreen* screen = self.window.screen ?: NSScreen.mainScreen;
    if (_ppm > 0 && screen == _ppmScreen) return _ppm;
    _ppmScreen = screen;
    _ppm = [self measurePointsPerMillimetre:screen];
    return _ppm;
}

- (float)measurePointsPerMillimetre:(NSScreen*)screen {
    NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
    if (number) {
        CGSize mm = CGDisplayScreenSize(number.unsignedIntValue);
        if (mm.width > 50) return (float)(screen.frame.size.width / mm.width);
    }
    return 72.f / 25.4f;
}

- (void)toggleLoupe:(id)sender {
    _loupe.on = !_loupe.on;
    if (!_loupe.on) _art->forget_zoom_layer();
    _loupeButton.state = _loupe.on ? NSControlStateValueOn : NSControlStateValueOff;
    [self showLoupePicks];
    save_loupe(_loupe);
    [self setNeedsDisplay:YES];
}
// The zoom and the size are picked in the strip, not in a context menu: in Logic's hosting
// process a pop-up menu left the editor deaf to the scroll wheel until it was reopened.
- (void)showLoupePicks {
    if (!_zoomPick || !_sizePick) return;
    int z = 1, d = 1;
    for (int i = 0; i < 3; i++) if (std::abs(_loupe.zoom - loupe_zooms[i]) < 0.05f) z = i;
    for (int i = 0; i < 4; i++) if (std::abs(_loupe.mm - loupe_sizes[i]) < 1) d = i;
    _zoomPick.selectedSegment = z;
    _sizePick.selectedSegment = d;
    _zoomPick.enabled = _sizePick.enabled = _loupe.on;
    [self placeStrip];
}
- (void)zoomPicked:(NSSegmentedControl*)pick { _loupe.zoom = loupe_zooms[std::max(0, std::min(2, (int)pick.selectedSegment))]; [self loupePicked]; }
- (void)sizePicked:(NSSegmentedControl*)pick { _loupe.mm = loupe_sizes[std::max(0, std::min(3, (int)pick.selectedSegment))]; [self loupePicked]; }
- (void)loupePicked {
    save_loupe(_loupe);
    [self.window makeFirstResponder:self];
    [self setNeedsDisplay:YES];
}

// The magnifier: a disc of the panel drawn again, enlarged and sharp, where the pointer is, or on
// the control being turned while it is turned.
- (void)drawLoupe:(CGContextRef)c backing:(float)backing dirty:(CGRect)dirty {
    // The pointer is read here, at every frame, not taken from mouse-moved events: in Logic those
    // stop coming after a context menu (the plug-in runs in a hosting process), and the magnifier
    // stayed frozen where the menu had been opened.
    if (CGRectIsEmpty(_lastLoupe) || !CGRectIntersectsRect(_lastLoupe, dirty)) return;
    float cx = (float)_loupeAt.x, cy = (float)_loupeAt.y;
    const float r = _lastLoupe.size.width / 2 - 16, z = _loupe.zoom;
    CGRect disc = CGRectMake(cx - r, cy - r, 2 * r, 2 * r);
    CGRect panel = CGRectMake(0, 0, _art->width, _art->height);

    CGContextSaveGState(c);
    CGContextClipToRect(c, panel);
    CGColorRef shade = CGColorCreateGenericGray(0, 0.7);
    CGContextSetShadowWithColor(c, CGSizeMake(0, 4), 12, shade);
    CGColorRelease(shade);
    CGContextSetRGBFillColor(c, 0.13f, 0.32f, 0.69f, 1);
    CGContextFillEllipseInRect(c, disc);
    CGContextRestoreGState(c);

    CGContextSaveGState(c);
    CGContextClipToRect(c, panel);
    CGContextAddEllipseInRect(c, disc);
    CGContextClip(c);
    // A panel point P shows at z * P - offset, so the pointer's point stays put; the offset is kept
    // on whole device pixels so the copied art stays sharp.
    float ox = roundf(cx * (z - 1) * backing) / backing, oy = roundf(cy * (z - 1) * backing) / backing;
    if (CGLayerRef zoomed = _art->zoomed_art(c, z, backing)) {
        CGContextSaveGState(c);
        CGContextTranslateCTM(c, -ox, -oy);
        CGContextTranslateCTM(c, 0, _art->height * z);
        CGContextScaleCTM(c, 1, -1);
        CGContextDrawLayerAtPoint(c, CGPointZero, zoomed);
        CGContextRestoreGState(c);
    }
    CGContextTranslateCTM(c, -ox, -oy);
    CGContextScaleCTM(c, z, z);
    CGRect seen = CGRectMake((CGRectGetMinX(disc) + ox) / z, (CGRectGetMinY(disc) + oy) / z, 2 * r / z, 2 * r / z);
    _art->draw_dynamic(c, backing * z, true, seen);
    CGContextRestoreGState(c);

    CGContextSaveGState(c);
    CGContextClipToRect(c, panel);
    CGContextSetRGBStrokeColor(c, 0.05f, 0.05f, 0.06f, 0.9f);
    CGContextSetLineWidth(c, 3);
    CGContextStrokeEllipseInRect(c, disc);
    CGContextSetRGBStrokeColor(c, 0.93f, 0.90f, 0.78f, 0.9f);
    CGContextSetLineWidth(c, 1.2f);
    CGContextStrokeEllipseInRect(c, CGRectInset(disc, 2, 2));
    CGContextRestoreGState(c);
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    if (!_art) return;
    _art->art_backing = 0;
    _art->lcd_valid = false;
}

- (void)paintDirty:(NSRect)dirty {
    CGContextRef c = [NSGraphicsContext currentContext].CGContext;
    float backing = self.window ? (float)self.window.backingScaleFactor : 2.f;
    _art->paint(c, backing, dirty);
    if (_loupe.on) [self drawLoupe:c backing:backing dirty:dirty];
    // The bar under the panel, with the magnifier's button and a hint.
    CGRect bar = CGRectMake(0, _art->height, _art->width, self.bounds.size.height - _art->height);
    CGContextSetRGBFillColor(c, 0.07f, 0.07f, 0.08f, 1);
    CGContextFillRect(c, bar);
    NSString* hint = _loupe.on ? @"Hover over the panel to enlarge it; it stays on a control while you turn it."
                               : @"Small print? Turn the magnifier on.";
    NSDictionary* attrs = @{NSFontAttributeName: [NSFont systemFontOfSize:11], NSForegroundColorAttributeName: [NSColor colorWithWhite:0.62 alpha:1]};
    NSSize size = [hint sizeWithAttributes:attrs];
    [hint drawAtPoint:NSMakePoint(NSMaxX((_sizePick ?: _lcdButton ?: _loupeButton).frame) + 12, _art->height + (strip_height - size.height) / 2) withAttributes:attrs];
}

- (NSPoint)at:(NSEvent*)event { return [self convertPoint:event.locationInWindow fromView:nil]; }

- (void)mouseDown:(NSEvent*)event {
    if (event.modifierFlags & NSEventModifierFlagControl) { [self rightMouseDown:event]; return; }
    NSPoint p = [self at:event];
    if (p.y >= _art->height) return;  // the bar under the panel
    _art->mouse_down(p.x, p.y);
    _dirty = YES;
}
- (void)mouseDragged:(NSEvent*)event {
    NSPoint p = [self at:event];
    _mouse = p;
    CGRect before = _art->drag_rect();
    _art->mouse_move(p.x, p.y);
    // Only the control being turned is redrawn while it turns, not the whole panel.
    CGRect now = _art->drag_rect();
    if (CGRectIsNull(now)) return;
    _partDirty = CGRectUnion(CGRectUnion(_partDirty, before), now);
}
- (void)mouseUp:(NSEvent*)event {
    _mouse = [self at:event];
    _art->mouse_up();
    _dirty = YES;
}

- (void)scrollWheel:(NSEvent*)event {
    // The trackpad's inertia goes on sending events after the fingers lift: it would turn the
    // encoder on its own. Only the gesture itself turns it, and a few detents an event at most.
    if (event.momentumPhase != NSEventPhaseNone) return;
    static float rest = 0;
    if (event.phase == NSEventPhaseBegan) rest = 0;
    rest += event.hasPreciseScrollingDeltas ? event.scrollingDeltaY / 12 : event.scrollingDeltaY;
    int detents = std::max(-2, std::min(2, (int)rest));
    if (!detents) return;
    rest = 0;
    _machine.engine->turn(detents);
    _art->dial_angle += 15.f * detents;
    _partDirty = CGRectUnion(_partDirty, _art->dial_rect());
}

- (void)rightMouseDown:(NSEvent*)event {
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"OB-12"];
    menu.autoenablesItems = NO;
    [[menu addItemWithTitle:@"Load .syx / .mid into MIDI IN…" action:@selector(loadSysex:) keyEquivalent:@""] setTarget:self];
    size_t sent = _machine.engine->sysex_sent();
    NSMenuItem* dump = [menu addItemWithTitle:[NSString stringWithFormat:@"Save MIDI OUT sysex as .syx (%zu bytes)…", sent] action:@selector(saveDump:) keyEquivalent:@""];
    dump.target = self;
    dump.enabled = sent > 0;
    [[menu addItemWithTitle:@"Reset memory to factory…" action:@selector(resetMemory:) keyEquivalent:@""] setTarget:self];
    int now_db = PanelView::output_gain_db();
    NSMenu* gain = [[NSMenu alloc] initWithTitle:@"Output gain"];
    for (int db = 0; db <= 24; db++) {
        NSMenuItem* item = [gain addItemWithTitle:db ? [NSString stringWithFormat:@"+%d dB", db] : @"0 dB" action:@selector(setGain:) keyEquivalent:@""];
        item.target = self;
        item.tag = db;
        item.state = db == now_db ? NSControlStateValueOn : NSControlStateValueOff;
    }
    NSMenuItem* gain_item = [menu addItemWithTitle:[NSString stringWithFormat:@"Output gain (now %@%d dB)", now_db ? @"+" : @"", now_db] action:nil keyEquivalent:@""];
    gain_item.submenu = gain;
    [menu addItem:[NSMenuItem separatorItem]];
    for (int percent : scale_percents) {
        NSMenuItem* item = [menu addItemWithTitle:[NSString stringWithFormat:@"%d%%", percent] action:@selector(setScale:) keyEquivalent:@""];
        item.target = self;
        item.tag = percent;
        item.state = std::abs(_art->width - PanelView::base_width * percent / 100) < 4 ? NSControlStateValueOn : NSControlStateValueOff;
    }
    [NSMenu popUpContextMenu:menu withEvent:event forView:self];
    // Back from the menu: take the events again (in the hosting process they may stay with it).
    [self.window makeFirstResponder:self];
    [self updateTrackingAreas];
}

- (void)setGain:(NSMenuItem*)item { write_gain((int)item.tag); }

- (void)setScale:(NSMenuItem*)item {
    int width = std::max(min_width, PanelView::base_width * (int)item.tag / 100);
    save_width(width);
    [self setFrameSize:NSMakeSize(width, lroundf(width / _art->aspect()) + strip_height)];
    [self setNeedsDisplay:YES];
}

- (void)loadSysex:(id)sender {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.allowedFileTypes = @[@"syx", @"mid", @"SYX", @"MID"];
    panel.allowsOtherFileTypes = YES;
    if ([panel runModal] != NSModalResponseOK || !panel.URL) return;
    std::vector<uint8_t> data;
    std::string problem = Engine::read_sysex(panel.URL.path.fileSystemRepresentation, data);
    if (!problem.empty()) {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = @"OB-12";
        alert.informativeText = [NSString stringWithUTF8String:problem.c_str()];
        [alert runModal];
        return;
    }
    _machine.engine->send_sysex(data.data(), (int)data.size());
}

- (void)saveDump:(id)sender {
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.nameFieldStringValue = @"OB-12 dump.syx";
    panel.allowedFileTypes = @[@"syx"];
    if ([panel runModal] != NSModalResponseOK || !panel.URL) return;
    std::vector<uint8_t> data;
    _machine.engine->sysex_sent(&data);
    std::ofstream f(panel.URL.path.fileSystemRepresentation, std::ios::binary);
    f.write((const char*)data.data(), (std::streamsize)data.size());
}

- (void)resetMemory:(id)sender {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Delete the stored memory (user programs, timbres, settings)?";
    alert.informativeText = @"The factory memory comes back the next time the plug-in loads.";
    [alert addButtonWithTitle:@"Delete"];
    [alert addButtonWithTitle:@"Cancel"];
    alert.alertStyle = NSAlertStyleWarning;
    if ([alert runModal] == NSAlertFirstButtonReturn) _machine.engine->forget_flash();
}

@end

// ---------------------------------------------------------------- the view factory

@interface OB12_VIEW_FACTORY : NSObject <AUCocoaUIBase>
@end

@implementation OB12_VIEW_FACTORY

- (unsigned)interfaceVersion { return 0; }

- (NSString*)description { return @"OB-12"; }

- (NSView*)uiViewForAudioUnit:(AudioUnit)unit withSize:(NSSize)size {
    void* plugin = nullptr;
    UInt32 bytes = sizeof plugin;
    ob12mac::MachineRef machine;
    if (AudioUnitGetProperty(unit, ob12mac::kPropertyMachine, kAudioUnitScope_Global, 0, &plugin, &bytes) != noErr ||
        !ob12mac::acquire(plugin, machine)) {
        NSTextField* note = [NSTextField labelWithString:@"OB-12: firmware not found.\nPut the rom folder in ~/Library/Application Support/OB12/rom."];
        [note setFrameSize:NSMakeSize(520, 60)];
        return note;
    }
    return [[OB12_PANEL_CLASS alloc] initWithMachine:machine];
}

@end
