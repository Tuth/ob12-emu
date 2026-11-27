// The colours of the display and the lamps, shared by the panels of every platform.
// 0 is the unit as built: the blue-white LCD with blue-violet dots and red lamps. The others are an
// OLED module fitted in its place, lit dots on black, with the lamps in the same colour. A user
// setting from the panel menu (display_skin.txt in the user folder), not something the firmware knows.
#pragma once

struct PanelSkin {
    const char* name;
    float gap[3], off[3], on[3];           // the display: between dots, an unlit dot, a lit dot; R G B, 0-255
    float glow[3], lamp[3], shine[3];      // a lit lamp: its halo, its body, its highlight; R G B, 0-1
};

constexpr PanelSkin panel_skins[] = {
    {"Original", {0xea, 0xec, 0xfa}, {0xdc, 0xdf, 0xf4}, {0x48, 0x46, 0xc0}, {1.f, 0.15f, 0.08f}, {1.f, 0.16f, 0.1f}, {1.f, 0.75f, 0.6f}},
    {"OLED green", {0x00, 0x00, 0x00}, {0x03, 0x0a, 0x05}, {0x2e, 0xf0, 0x62}, {0.1f, 1.f, 0.3f}, {0.18f, 0.94f, 0.38f}, {0.72f, 1.f, 0.8f}},
    {"OLED amber", {0x00, 0x00, 0x00}, {0x0b, 0x07, 0x02}, {0xff, 0xae, 0x1c}, {1.f, 0.62f, 0.05f}, {1.f, 0.68f, 0.11f}, {1.f, 0.9f, 0.66f}},
    {"OLED red", {0x00, 0x00, 0x00}, {0x0b, 0x03, 0x02}, {0xff, 0x34, 0x24}, {1.f, 0.15f, 0.08f}, {1.f, 0.16f, 0.1f}, {1.f, 0.75f, 0.6f}},
    {"OLED white", {0x00, 0x00, 0x00}, {0x07, 0x08, 0x0a}, {0xf2, 0xf5, 0xff}, {0.85f, 0.9f, 1.f}, {0.95f, 0.96f, 1.f}, {1.f, 1.f, 1.f}},
};
constexpr int panel_skin_count = sizeof panel_skins / sizeof panel_skins[0];
