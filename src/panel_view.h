// The front panel as a child window, drawn by Direct2D. Shared by the player and the VST3 editor.
#pragma once
#include <functional>
#include <string>

class Engine;

class PanelView {
public:
    PanelView(Engine& engine, const std::string& art_file);
    ~PanelView();
    bool attach(void* parent_hwnd);
    void detach();
    void resize(int width, int height);
    float aspect() const;  // width over height of the art
    // The right click menu offers sizes; the owner resizes the window and calls resize().
    void on_width_request(std::function<void(int width)> request);
    static constexpr int base_width = 1500;  // the 100% size

private:
    struct Impl;
    Impl* impl;
};
