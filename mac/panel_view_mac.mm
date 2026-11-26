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
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
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

namespace {

struct Run {
    uint32_t argb;
    uint8_t mode;  // 1 fill (no antialias), 2 fill even-odd, 3 stroke, 4 push clip, 5 pop clip
    float width;
    std::vector<uint8_t> kinds;
    std::vector<float> points;
    CGPathRef path = nullptr;
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
    }

    void load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        std::vector<char> data((std::istreambuf_iterator<char>(f)), {});
        if (data.size() < 16 || memcmp(data.data(), "OB12", 4)) return;
        const char* p = data.data() + 4;
        auto u32 = [&]() { uint32_t v; memcpy(&v, p, 4); p += 4; return v; };
        auto f32 = [&]() { float v; memcpy(&v, p, 4); p += 4; return v; };
        uint32_t count = u32();
        art_w = f32();
        art_h = f32();
        runs.resize(count);
        for (auto& r : runs) {
            r.argb = u32();
            r.mode = (uint8_t)*p++;
            r.width = f32();
            uint32_t commands = u32();
            r.kinds.reserve(commands);
            for (uint32_t i = 0; i < commands; i++) {
                uint8_t kind = (uint8_t)*p++;
                r.kinds.push_back(kind);
                int n = kind == 2 ? 3 : kind == 3 ? 0 : 1;
                for (int j = 0; j < n * 2; j++) r.points.push_back(f32());
            }
        }
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

    void draw_moving(CGContextRef c) {
        int count = sizeof panel_pots / sizeof panel_pots[0];
        float scale = art_scale();
        const float f = sx();
        for (int i = 0; i < count; i++) {
            const PanelPot& p = panel_pots[i];
            if (!p.slider || !moving[i].found) continue;
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
            if (!m.found) continue;
            float value = pot_value[i] / 255.f;
            draw_part(m, panel_pots[i].slider, value, m.lo + value * (m.hi - m.lo) - m.rest);
        }
        if (dial.found) draw_part(dial, false, 0, dial_angle);
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
        bool fresh = engine.lcd(lcd_bits);
        if (!fresh && !lcd_valid) return;
        bool changed = fresh && memcmp(lcd_bits, lcd_shown, sizeof lcd_bits) != 0;
        if (!lcd_image || w != lcd_w || h != lcd_h) changed = true;
        if (changed) {
            if (fresh) memcpy(lcd_shown, lcd_bits, sizeof lcd_bits);
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

    void paint(CGContextRef c, float backing) {
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
        float s = sx();
        CGContextSaveGState(c);
        CGContextTranslateCTM(c, left(), 0);
        draw_lcd(c, backing);
        draw_moving(c);
        uint8_t latches[16];
        engine.leds(latches);
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

// ---------------------------------------------------------------- the NSView

@interface OB12_PANEL_CLASS : NSView
- (instancetype)initWithMachine:(ob12mac::MachineRef)machine;
@end

@implementation OB12_PANEL_CLASS {
    ob12mac::MachineRef _machine;
    std::unique_ptr<PanelArt> _art;
    NSTimer* _timer;
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
    self = [super initWithFrame:NSMakeRect(0, 0, w, h)];
    if (self) {
        _machine = machine;
        _art = std::move(art);
        _art->width = w;
        _art->height = h;
        __weak OB12_PANEL_CLASS* weak = self;
        _timer = [NSTimer timerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer*) { [weak setNeedsDisplay:YES]; }];
        [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
    }
    return self;
}

- (void)dealloc {
    [_timer invalidate];
}

- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    if (!_art) return;
    _art->width = (int)size.width;
    _art->height = (int)size.height;
    _art->art_backing = 0;  // the art is drawn again at the new size
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    if (!_art) return;
    _art->art_backing = 0;
    _art->lcd_valid = false;
}

- (void)drawRect:(NSRect)dirty {
    CGContextRef c = [NSGraphicsContext currentContext].CGContext;
    float backing = self.window ? (float)self.window.backingScaleFactor : 2.f;
    _art->paint(c, backing);
}

- (NSPoint)at:(NSEvent*)event { return [self convertPoint:event.locationInWindow fromView:nil]; }

- (void)mouseDown:(NSEvent*)event {
    if (event.modifierFlags & NSEventModifierFlagControl) { [self rightMouseDown:event]; return; }
    NSPoint p = [self at:event];
    _art->mouse_down(p.x, p.y);
    [self setNeedsDisplay:YES];
}
- (void)mouseDragged:(NSEvent*)event {
    NSPoint p = [self at:event];
    _art->mouse_move(p.x, p.y);
    [self setNeedsDisplay:YES];
}
- (void)mouseUp:(NSEvent*)event {
    _art->mouse_up();
    [self setNeedsDisplay:YES];
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
    [self setNeedsDisplay:YES];
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
}

- (void)setGain:(NSMenuItem*)item { write_gain((int)item.tag); }

- (void)setScale:(NSMenuItem*)item {
    int width = std::max(min_width, PanelView::base_width * (int)item.tag / 100);
    save_width(width);
    [self setFrameSize:NSMakeSize(width, lroundf(width / _art->aspect()))];
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
