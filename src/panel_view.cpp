#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>
#include "ob12_engine.h"
#include "panel_controls.h"
#include "panel_view.h"
#pragma comment(lib, "d2d1.lib")

namespace {

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Run {
    uint32_t argb;
    uint8_t mode;
    float width;
    std::vector<uint8_t> kinds;
    std::vector<float> points;
    ID2D1PathGeometry* geometry = nullptr;
};

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
    ID2D1HwndRenderTarget* target = nullptr;
    ID2D1Bitmap* art = nullptr;       // the panel at the current size
    ID2D1Bitmap* lcd = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    std::vector<Run> runs;
    float art_w = 472, art_h = 126;
    int width = 0, height = 0;

    int pot_value[sizeof panel_pots / sizeof panel_pots[0]];
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
        if (FAILED(factory->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(window, D2D1::SizeU(rc.right, rc.bottom)), &target))) return;
        target->CreateSolidColorBrush(D2D1::ColorF(0), &brush);
        target->CreateBitmap(D2D1::SizeU(240, 64), D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)), &lcd);
        render_art();
    }

    void drop_target() {
        release(art); release(lcd); release(brush); release(target);
    }

    float sx() const { return std::min(width / art_w, height / art_h) * art_w / panel_frame_w; }

    void paint() {
        make_target();
        if (!target) return;
        target->BeginDraw();
        target->Clear(D2D1::ColorF(0.93f, 0.93f, 0.92f));
        if (art) target->DrawBitmap(art);
        float s = sx();

        if (engine.lcd(lcd_bits) && lcd) {
            static uint32_t pixels[240 * 64];
            for (int y = 0; y < 64; y++) for (int x = 0; x < 240; x++) {
                bool on = lcd_bits[y * 30 + (x >> 3)] & (0x80 >> (x & 7));
                pixels[y * 240 + x] = on ? 0xff10203a : 0xffb9c8d8;
            }
            lcd->CopyFromMemory(nullptr, pixels, 240 * 4);
            target->DrawBitmap(lcd, D2D1::RectF(panel_lcd_x0 * s, panel_lcd_y0 * s, panel_lcd_x1 * s, panel_lcd_y1 * s), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        }

        // Current values over the art: a pointer on knobs, a bar on sliders.
        brush->SetColor(D2D1::ColorF(0.85f, 0.1f, 0.1f));
        int count = sizeof panel_pots / sizeof panel_pots[0];
        for (int i = 0; i < count; i++) {
            const PanelPot& p = panel_pots[i];
            float value = pot_value[i] / 255.f;
            if (p.slider) {
                float y = (p.y + 52 - value * 104) * s;
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
        case WM_MOUSEWHEEL: self->engine.turn(GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA); return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
};

PanelView::PanelView(Engine& engine, const std::string& art_file) : impl(new Impl(engine)) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &impl->factory);
    impl->load(art_file);
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
    if (impl->target) {
        impl->target->Resize(D2D1::SizeU(width, height));
        impl->render_art();
    }
    InvalidateRect(impl->window, nullptr, FALSE);
}

float PanelView::aspect() const { return impl->art_w / impl->art_h; }
