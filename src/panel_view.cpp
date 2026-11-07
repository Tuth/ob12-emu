#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <wincodec.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>
#include "ob12_engine.h"
#include "panel_controls.h"
#include "panel_view.h"
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace {

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Run {
    uint32_t argb;
    uint8_t mode;
    float width;
    std::vector<uint8_t> kinds;
    std::vector<float> points;
    ID2D1PathGeometry* geometry = nullptr;
    bool moving = false;  // drawn per frame by its control, not in the static art
};

// The parts of a control that move with its value: a slider's cap, a knob's face marks.
// Found in the vector art at load. Units are the art's own.
struct Moving {
    std::vector<int> runs;
    float cx = 0, cy = 0;   // knob centre
    float rest = 0;         // knob: the art's pointer angle, degrees clockwise from 12 o'clock
    float travel = 0;       // slider: art units from value 0 to full
    float lo = -135, hi = 135;  // knob: angles of its end marks, degrees clockwise from 12 o'clock
    bool found = false;
};

constexpr int scale_command = 1000;  // menu id is this plus the percentage
constexpr int shot_command = 1999;   // hidden: save the panel to OB12_PANEL_SHOT as PNG
constexpr int scale_percents[] = {50, 67, 75, 100, 125, 150, 200};

D2D1_COLOR_F color(uint32_t argb) {
    return D2D1::ColorF(((argb >> 16) & 255) / 255.f, ((argb >> 8) & 255) / 255.f, (argb & 255) / 255.f, (argb >> 24) / 255.f);
}

const wchar_t* class_name = L"OB12PanelView";
const UINT_PTR frame_timer = 1;

}  // namespace

struct PanelView::Impl {
    Engine& engine;
    HWND window = nullptr;
    ID2D1Factory* factory = nullptr;
    ID2D1RenderTarget* target = nullptr;        // the window's, or a WIC bitmap's for save_image
    ID2D1HwndRenderTarget* hwnd_target = nullptr;
    ID2D1Bitmap* art = nullptr;       // the panel at the current size
    ID2D1Bitmap* lcd = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    std::vector<Run> runs;
    float art_w = 472, art_h = 126;
    std::function<void(int)> width_request;
    int width = 0, height = 0;

    int pot_value[sizeof panel_pots / sizeof panel_pots[0]];
    Moving moving[sizeof panel_pots / sizeof panel_pots[0]];
    std::vector<ID2D1Layer*> live_layers;
    int drag_pot = -1, drag_start_y = 0, drag_start_value = 0, down_key = -1;
    bool drag_dial = false;
    int dial_y = 0;
    uint8_t lcd_bits[30 * 64];

    explicit Impl(Engine& e) : engine(e) {
        for (int& v : pot_value) v = 0x80;
        memset(lcd_bits, 0, sizeof lcd_bits);
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
    }

    void box(const Run& r, float& x0, float& y0, float& x1, float& y1) const {
        x0 = y0 = 1e9f; x1 = y1 = -1e9f;
        for (size_t i = 0; i + 1 < r.points.size(); i += 2) {
            x0 = std::min(x0, r.points[i]); x1 = std::max(x1, r.points[i]);
            y0 = std::min(y0, r.points[i + 1]); y1 = std::max(y1, r.points[i + 1]);
        }
    }

    void find_moving() {
        const float k = art_w / panel_frame_w;  // frame units to art units
        int count = sizeof panel_pots / sizeof panel_pots[0];
        for (int i = 0; i < count; i++) {
            const PanelPot& p = panel_pots[i];
            Moving& m = moving[i];
            float px = p.x * k, py = p.y * k;
            int first = -1, last = -1;
            std::vector<int> picked;
            if (p.slider) {
                // The cap: below the track, within the cap's width, not the ticks left of the track.
                for (int r = 0; r < (int)runs.size(); r++) {
                    if (runs[r].mode >= 4 || runs[r].points.empty()) continue;
                    float x0, y0, x1, y1;
                    box(runs[r], x0, y0, x1, y1);
                    if (x0 >= px - 14 * k && x1 <= px + 14 * k && y0 >= py + 36 * k && y1 <= py + 92 * k) picked.push_back(r);
                }
                m.travel = 88 * k;
            } else {
                // The body: a grey fill whose box centre is the knob centre.
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
                    // Some knobs (Osc2 Range) have no body fill: take the centre from the knurled ring.
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
                float radius = (by1 - by0) / 2;  // the height: Bezier handles widen the box sideways
                double sx = 0, sy = 0;  // the face marks' fill centroid points where the knob points
                long sn = 0;
                for (int r = 0; r < (int)runs.size(); r++) {
                    const Run& run = runs[r];
                    if (r == body || run.mode >= 4 || run.points.empty() || (run.argb == 0xffbcbec0 && runs[body].mode == 2)) continue;
                    int figures = (int)std::count(run.kinds.begin(), run.kinds.end(), (uint8_t)0);
                    if (figures > 8) continue;  // the knurled ring
                    bool inside = true;  // every point of the shape on the knob's face
                    for (size_t j = 0; j + 1 < run.points.size() && inside; j += 2)
                        if (std::hypot(run.points[j] - m.cx, run.points[j + 1] - m.cy) > radius * 1.02f) inside = false;
                    if (!inside) continue;
                    picked.push_back(r);
                    // the pointer's dark fill only
                    if (run.mode == 2 && run.argb == 0xff6d6e71) for (size_t j = 0; j + 1 < run.points.size(); j += 2) { sx += run.points[j] - m.cx; sy += run.points[j + 1] - m.cy; sn++; }
                }
                // The scale's end marks: the small dark fills just outside the knob, below its centre.
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
                if (!sn) for (int r : picked) if (runs[r].mode == 2)  // no dark pointer: any face fill
                    for (size_t j = 0; j + 1 < runs[r].points.size(); j += 2) { sx += runs[r].points[j] - m.cx; sy += runs[r].points[j + 1] - m.cy; sn++; }
                if (sn) m.rest = (float)(atan2(sx, -sy) * 180 / 3.14159265);
            }
            if (picked.empty()) continue;
            first = picked.front(); last = picked.back();
            // Clip pairs nested between them belong to the part too; an unmatched push or pop belongs to
            // an outer clip and stays in the static art, so its layers stay balanced.
            for (int r : picked) { runs[r].moving = true; m.runs.push_back(r); }
            std::vector<int> open;
            for (int r = first; r <= last; r++) {
                if (runs[r].mode == 4) open.push_back(r);
                else if (runs[r].mode == 5 && !open.empty()) {
                    int push = open.back();
                    open.pop_back();
                    bool whole = true;  // only a clip whose whole content moves goes with it
                    for (int q = push + 1; q < r; q++) if (!runs[q].moving && runs[q].mode < 4) whole = false;
                    if (!whole) continue;
                    runs[push].moving = runs[r].moving = true;
                    m.runs.push_back(push);
                    m.runs.push_back(r);
                }
            }
            std::sort(m.runs.begin(), m.runs.end());
            m.found = true;
            if (getenv("OB12_PANEL_DEBUG")) fprintf(stderr, "%-18s runs %3zu centre %.1f %.1f rest %.0f range %.0f..%.0f\n", p.name, m.runs.size(), m.cx / k, m.cy / k, m.rest, m.lo, m.hi);
        }
    }

    // A control's moving parts at its value, drawn over the static art.
    void draw_moving(float scale) {
        int count = sizeof panel_pots / sizeof panel_pots[0];
        ID2D1SolidColorBrush* paint = brush;
        // The art's rails stop where the resting cap hides them; carry each down to as far below the
        // bottom tick as the rail reaches above the top one, with its slot. Frame units, then scaled.
        const float f = scale * art_w / panel_frame_w;
        for (int i = 0; i < count; i++) {
            const PanelPot& p = panel_pots[i];
            if (!p.slider || !moving[i].found) continue;
            float top = p.y + 43.9f, bottom = p.y + 81;
            paint->SetColor(D2D1::ColorF(0x231f20));
            target->FillRectangle(D2D1::RectF((p.x - 0.9f) * f, (p.y + 43) * f, (p.x + 0.8f) * f, (bottom - 2.1f) * f), paint);
            paint->SetColor(D2D1::ColorF(0x000202));
            float w = std::max(0.5f, 1.01f * f);
            target->DrawLine(D2D1::Point2F((p.x - 3.7f) * f, top * f), D2D1::Point2F((p.x - 3.7f) * f, bottom * f), paint, w);
            target->DrawLine(D2D1::Point2F((p.x + 3.7f) * f, top * f), D2D1::Point2F((p.x + 3.7f) * f, bottom * f), paint, w);
            target->DrawLine(D2D1::Point2F((p.x - 3.7f) * f, bottom * f), D2D1::Point2F((p.x + 3.7f) * f, bottom * f), paint, w);
        }
        for (int i = 0; i < count; i++) {
            const Moving& m = moving[i];
            if (!m.found) continue;
            float value = pot_value[i] / 255.f;
            D2D1::Matrix3x2F place = panel_pots[i].slider
                ? D2D1::Matrix3x2F::Translation(0, -value * m.travel)
                : D2D1::Matrix3x2F::Rotation(m.lo + value * (m.hi - m.lo) - m.rest, D2D1::Point2F(m.cx, m.cy));
            target->SetTransform(place * D2D1::Matrix3x2F::Scale(scale, scale));
            size_t depth = 0;
            for (int r : m.runs) {
                const Run& run = runs[r];
                if (run.mode == 4) {
                    if (depth == live_layers.size()) { ID2D1Layer* l = nullptr; target->CreateLayer(&l); live_layers.push_back(l); }
                    target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), run.geometry), live_layers[depth++]);
                } else if (run.mode == 5) {
                    if (depth) { target->PopLayer(); depth--; }
                } else {
                    paint->SetColor(color(run.argb));
                    if (run.mode == 3) target->DrawGeometry(run.geometry, paint, run.width);
                    else target->FillGeometry(run.geometry, paint);
                }
            }
            while (depth) { target->PopLayer(); depth--; }
        }
        target->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    void build_geometry() {
        for (auto& r : runs) {
            if (r.geometry || r.mode == 5) continue;
            factory->CreatePathGeometry(&r.geometry);
            ID2D1GeometrySink* sink = nullptr;
            r.geometry->Open(&sink);
            sink->SetFillMode(r.mode == 2 ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING);
            const float* q = r.points.data();
            bool open = false;
            D2D1_FIGURE_BEGIN begin = r.mode == 3 ? D2D1_FIGURE_BEGIN_HOLLOW : D2D1_FIGURE_BEGIN_FILLED;
            for (uint8_t kind : r.kinds) {
                switch (kind) {
                case 0:
                    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    sink->BeginFigure(D2D1::Point2F(q[0], q[1]), begin);
                    open = true;
                    q += 2;
                    break;
                case 1:
                    sink->AddLine(D2D1::Point2F(q[0], q[1]));
                    q += 2;
                    break;
                case 2:
                    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(q[0], q[1]), D2D1::Point2F(q[2], q[3]), D2D1::Point2F(q[4], q[5])));
                    q += 6;
                    break;
                case 3:
                    if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    open = false;
                    break;
                }
            }
            if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();
        }
    }

    void render_art() {
        release(art);
        if (!target || width < 8 || height < 8) return;
        ID2D1BitmapRenderTarget* layer = nullptr;
        if (FAILED(target->CreateCompatibleRenderTarget(D2D1::SizeF((float)width, (float)height), &layer))) return;
        ID2D1SolidColorBrush* paint = nullptr;
        layer->CreateSolidColorBrush(D2D1::ColorF(0), &paint);
        layer->BeginDraw();
        layer->Clear(D2D1::ColorF(0.93f, 0.93f, 0.92f));
        float scale = std::min(width / art_w, height / art_h);
        layer->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
        std::vector<ID2D1Layer*> clips;
        size_t depth = 0;
        for (auto& r : runs) {
            if (r.moving) continue;
            if (r.mode == 4) {
                if (depth == clips.size()) {
                    ID2D1Layer* l = nullptr;
                    layer->CreateLayer(&l);
                    clips.push_back(l);
                }
                layer->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), r.geometry), clips[depth++]);
            } else if (r.mode == 5) {
                layer->PopLayer();
                depth--;
            } else {
                paint->SetColor(color(r.argb));
                if (r.mode == 3) layer->DrawGeometry(r.geometry, paint, r.width);
                else layer->FillGeometry(r.geometry, paint);
            }
        }
        layer->EndDraw();
        for (auto* l : clips) l->Release();
        layer->GetBitmap(&art);
        paint->Release();
        layer->Release();
    }

    void make_target() {
        if (target || !window) return;
        RECT rc;
        GetClientRect(window, &rc);
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
        props.dpiX = props.dpiY = 96;  // one unit is one pixel
        if (FAILED(factory->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(window, D2D1::SizeU(rc.right, rc.bottom)), &hwnd_target))) return;
        target = hwnd_target;
        target->CreateSolidColorBrush(D2D1::ColorF(0), &brush);
        lcd_valid = false;  // the LCD bitmap is made at its device size in draw_lcd
        render_art();
    }

    void drop_target() {
        for (auto* l : live_layers) l->Release();
        live_layers.clear();
        release(art); release(lcd); release(brush); release(target);
        hwnd_target = nullptr;
    }

    void scale_menu(LPARAM at) {
        if (!width_request) return;
        HMENU menu = CreatePopupMenu();
        for (int percent : scale_percents) {
            wchar_t text[16];
            swprintf(text, 16, L"%d%%", percent);
            bool now = std::abs(width - PanelView::base_width * percent / 100) < 4;
            AppendMenuW(menu, MF_STRING | (now ? MF_CHECKED : 0), scale_command + percent, text);
        }
        POINT p = {GET_X_LPARAM(at), GET_Y_LPARAM(at)};
        ClientToScreen(window, &p);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, p.x, p.y, 0, window, nullptr);
        DestroyMenu(menu);
    }

    // The LCD as a dot matrix, like the EII panel: every pixel a dot of the same size with a gap,
    // computed at the exact device size and drawn 1:1, so no column is doubled by scaling.
    // A pixel's colour is the area of each dot it covers, so dots stay even at any scale.
    std::vector<uint32_t> lcd_pixels;
    uint8_t lcd_shown[30 * 64] = {};
    int lcd_w = 0, lcd_h = 0;
    bool lcd_valid = false;

    // Per device column (or row): which LCD cells it overlaps and by how much of a dot.
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

    void draw_lcd(float s) {
        int x0 = (int)lroundf(panel_lcd_x0 * s), y0 = (int)lroundf(panel_lcd_y0 * s);
        int w = (int)lroundf(panel_lcd_x1 * s) - x0, h = (int)lroundf(panel_lcd_y1 * s) - y0;
        if (w < 8 || h < 4) return;
        bool fresh = engine.lcd(lcd_bits);
        if (!fresh && !lcd_valid) return;
        bool changed = fresh && memcmp(lcd_bits, lcd_shown, sizeof lcd_bits) != 0;
        if (!lcd || w != lcd_w || h != lcd_h) {
            release(lcd);
            if (FAILED(target->CreateBitmap(D2D1::SizeU(w, h), D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)), &lcd))) return;
            lcd_w = w; lcd_h = h;
            changed = true;
        }
        if (changed || !lcd_valid) {
            if (fresh) memcpy(lcd_shown, lcd_bits, sizeof lcd_bits);
            std::vector<int> fx, fy;
            std::vector<float> cx, cy;
            dot_cover(w, 240, fx, cx);
            dot_cover(h, 64, fy, cy);
            // A set bit is background. Gap, unlit dot and lit dot colours, R G B.
            const float gap[3] = {0xd2, 0xde, 0xe8}, off[3] = {0xc4, 0xd2, 0xde}, on[3] = {0x10, 0x20, 0x3a};
            lcd_pixels.assign((size_t)w * h, 0);
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
                uint32_t c = 0xff000000;
                for (int k = 0; k < 3; k++) {
                    float v = gap[k] * rest + off[k] * unlit + on[k] * lit;
                    c |= (uint32_t)std::min(255.f, v + 0.5f) << (16 - 8 * k);
                }
                lcd_pixels[(size_t)py * w + px] = c;
            }
            lcd->CopyFromMemory(nullptr, lcd_pixels.data(), w * 4);
            lcd_valid = true;
        }
        target->DrawBitmap(lcd, D2D1::RectF((float)x0, (float)y0, (float)(x0 + w), (float)(y0 + h)), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    }

    // The panel drawn into a WIC bitmap and saved as PNG, off screen: for checking it by eye.
    bool save_image(const char* path) {
        if (const char* v = getenv("OB12_PANEL_SHOT_VALUE"))  // study: every pot at this value, 0-255
            for (int& value : pot_value) value = atoi(v);
        drop_target();
        IWICImagingFactory* wic = nullptr;
        IWICBitmap* bitmap = nullptr;
        bool ok = false;
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
            SUCCEEDED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) {
            D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            if (SUCCEEDED(factory->CreateWicBitmapRenderTarget(bitmap, props, &target))) {
                target->CreateSolidColorBrush(D2D1::ColorF(0), &brush);
                lcd_valid = false;
                render_art();
                paint();
                drop_target();
                wchar_t wide[MAX_PATH];
                MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH);
                IWICStream* stream = nullptr;
                IWICBitmapEncoder* encoder = nullptr;
                IWICBitmapFrameEncode* frame = nullptr;
                if (SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(wide, GENERIC_WRITE)) &&
                    SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) && SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
                    SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr))) {
                    frame->SetSize(width, height);
                    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
                    frame->SetPixelFormat(&format);
                    ok = SUCCEEDED(frame->WriteSource(bitmap, nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
                }
                release(frame); release(encoder); release(stream);
            }
        }
        release(bitmap); release(wic);
        if (window) InvalidateRect(window, nullptr, FALSE);
        return ok;
    }

    float sx() const { return std::min(width / art_w, height / art_h) * art_w / panel_frame_w; }

    void paint() {
        make_target();
        if (!target) return;
        target->BeginDraw();
        target->Clear(D2D1::ColorF(0.93f, 0.93f, 0.92f));
        if (art) target->DrawBitmap(art);
        float s = sx();

        draw_lcd(s);

        if (getenv("OB12_PANEL_BARE")) { target->EndDraw(); return; }  // study: the art alone, for finding controls
        // Caps and knob faces move with their values; a red mark only where the art had none found.
        draw_moving(std::min(width / art_w, height / art_h));
        // Lamps: lit from the Master's LED latches. A soft glow, then the lamp, then a highlight.
        uint8_t latches[16];
        engine.leds(latches);
        for (const PanelLed& led : panel_leds) {
            if (!(latches[led.latch] >> led.bit & 1)) continue;
            D2D1_POINT_2F c = D2D1::Point2F(led.x * s, led.y * s);
            float r = led.lamp ? 5.6f : 3.1f;  // a key's lamp, or a small indicator beside a label
            brush->SetColor(D2D1::ColorF(1.f, 0.15f, 0.08f, 0.28f));
            target->FillEllipse(D2D1::Ellipse(c, r * 1.6f * s, r * 1.6f * s), brush);
            brush->SetColor(D2D1::ColorF(1.f, 0.16f, 0.1f));
            target->FillEllipse(D2D1::Ellipse(c, r * s, r * s), brush);
            brush->SetColor(D2D1::ColorF(1.f, 0.75f, 0.6f, 0.85f));
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.x - r * 0.29f * s, c.y - r * 0.32f * s), r * 0.32f * s, r * 0.25f * s), brush);
        }
        brush->SetColor(D2D1::ColorF(0.85f, 0.1f, 0.1f));
        int count = sizeof panel_pots / sizeof panel_pots[0];
        for (int i = 0; i < count; i++) {
            const PanelPot& p = panel_pots[i];
            if (moving[i].found) continue;
            float value = pot_value[i] / 255.f;
            if (p.slider) {
                float y = (p.y + 44 - value * 88) * s;  // the art's track is 88 units long
                target->DrawLine(D2D1::Point2F((p.x - 12) * s, y), D2D1::Point2F((p.x + 12) * s, y), brush, std::max(1.5f, 4 * s));
            } else {
                float angle = (-135 + value * 270) * 3.14159265f / 180;
                float r = 22 * s;
                target->DrawLine(D2D1::Point2F(p.x * s, p.y * s), D2D1::Point2F(p.x * s + r * sinf(angle), p.y * s - r * cosf(angle)), brush, std::max(1.5f, 3.5f * s));
            }
        }
        if (down_key >= 0) {
            const PanelKey& k = panel_keys[down_key];
            brush->SetColor(D2D1::ColorF(0.85f, 0.1f, 0.1f, 0.45f));
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(k.x * s, k.y * s), 21 * s, 12 * s), brush);
        }
        if (target->EndDraw() == D2DERR_RECREATE_TARGET) drop_target();
    }

    void set_pot(int i, int value) {
        value = std::max(0, std::min(255, value));
        pot_value[i] = value;
        engine.set_pot(panel_pots[i].channel, panel_pots[i].mux, value);
    }

    void mouse_down(int x, int y) {
        float s = sx();
        float fx = x / s, fy = y / s;
        SetCapture(window);
        int count = sizeof panel_pots / sizeof panel_pots[0];
        for (int i = 0; i < count; i++) {
            const PanelPot& p = panel_pots[i];
            bool hit = p.slider ? fabsf(fx - p.x) < 20 && fabsf(fy - p.y) < 62 : hypotf(fx - p.x, fy - p.y) < 30;
            if (hit) { drag_pot = i; drag_start_y = y; drag_start_value = pot_value[i]; return; }
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
        if (hypotf(fx - panel_dial_x, fy - panel_dial_y) < panel_dial_r) { drag_dial = true; dial_y = y; }
    }

    void mouse_move(int y) {
        if (drag_pot >= 0) set_pot(drag_pot, drag_start_value + (drag_start_y - y) * 256 / std::max(60, height / 3));
        if (drag_dial) {
            int detents = (dial_y - y) / 6;
            if (detents) { engine.turn(detents); dial_y -= detents * 6; }
        }
    }

    void mouse_up() {
        if (down_key >= 0) engine.key(panel_keys[down_key].bar, panel_keys[down_key].bit, false);
        down_key = drag_pot = -1;
        drag_dial = false;
        ReleaseCapture();
    }

    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = (Impl*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (self) switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            self->paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_TIMER: InvalidateRect(hwnd, nullptr, FALSE); return 0;
        case WM_LBUTTONDOWN: self->mouse_down(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)); return 0;
        case WM_MOUSEMOVE: self->mouse_move(GET_Y_LPARAM(lparam)); return 0;
        case WM_LBUTTONUP: self->mouse_up(); return 0;
        case WM_RBUTTONUP: self->scale_menu(lparam); return 0;
        case WM_COMMAND: {
            int percent = (int)LOWORD(wparam) - scale_command;
            if (LOWORD(wparam) == shot_command) { if (const char* path = getenv("OB12_PANEL_SHOT")) self->save_image(path); return 0; }
            if (percent > 0 && percent <= 400 && self->width_request) self->width_request(PanelView::base_width * percent / 100);
            return 0;
        }
        case WM_MOUSEWHEEL: self->engine.turn(GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA); return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
};

PanelView::PanelView(Engine& engine, const std::string& art_file) : impl(new Impl(engine)) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &impl->factory);
    impl->load(art_file);
    impl->find_moving();
    impl->build_geometry();
}

PanelView::~PanelView() {
    detach();
    for (auto& r : impl->runs) release(r.geometry);
    release(impl->factory);
    delete impl;
}

bool PanelView::attach(void* parent) {
    static bool registered = false;
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&Impl::proc, &module);
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = Impl::proc;
        wc.hInstance = module;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = class_name;
        RegisterClassW(&wc);
        registered = true;
    }
    RECT rc;
    GetClientRect((HWND)parent, &rc);
    impl->window = CreateWindowExW(0, class_name, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, rc.right, rc.bottom, (HWND)parent, nullptr, module, nullptr);
    if (!impl->window) return false;
    SetWindowLongPtrW(impl->window, GWLP_USERDATA, (LONG_PTR)impl);
    impl->width = rc.right;
    impl->height = rc.bottom;
    SetTimer(impl->window, frame_timer, 33, nullptr);
    return true;
}

void PanelView::detach() {
    if (!impl->window) return;
    KillTimer(impl->window, frame_timer);
    impl->drop_target();
    DestroyWindow(impl->window);
    impl->window = nullptr;
}

void PanelView::resize(int width, int height) {
    impl->width = width;
    impl->height = height;
    if (!impl->window) return;
    SetWindowPos(impl->window, nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    if (impl->hwnd_target) {
        impl->hwnd_target->Resize(D2D1::SizeU(width, height));
        impl->render_art();
    }
    InvalidateRect(impl->window, nullptr, FALSE);
}

float PanelView::aspect() const { return impl->art_w / impl->art_h; }

void PanelView::on_width_request(std::function<void(int)> request) { impl->width_request = std::move(request); }
