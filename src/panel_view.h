// The front panel as a child window, drawn by Direct2D. Shared by the player and the VST3 editor.
#pragma once
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

private:
    struct Impl;
    Impl* impl;
};
