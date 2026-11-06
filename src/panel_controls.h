// Controls on the panel art. x and y are in a 2576 x 687 frame of the cropped manual page.
// channel and mux from the Panel A and B potentiometer sheets, bar and bit from the button sheets.
// Positions were read off the image by eye. Where one sheet label covers several controls, as with
// the three Wave Mix sliders and Wave Select buttons, the left to right order is assumed.
#pragma once

struct PanelPot { const char* name; float x, y; bool slider; int channel, mux; };
struct PanelKey { const char* name; float x, y; int bar, bit; };

static const float panel_frame_w = 2576, panel_frame_h = 687;

static const PanelPot panel_pots[] = {
    {"LFO1 Rate", 113, 77, false, 1, 7}, {"LFO1 Fade", 113, 165, false, 1, 5},
    {"Osc2 Level", 260, 105, true, 1, 6}, {"Ring Mod", 313, 105, true, 1, 4}, {"Noise", 366, 105, true, 1, 2},
    {"Common Attack", 447, 105, true, 2, 5}, {"Common Decay", 499, 105, true, 2, 7}, {"Common Env Depth", 580, 165, false, 2, 6},
    {"Osc1 Mix 1", 700, 105, true, 2, 4}, {"Osc1 Mix 2", 753, 105, true, 3, 0}, {"Osc1 Mix 3", 805, 105, true, 3, 3},
    {"Osc1 FM", 888, 75, false, 3, 7}, {"Osc1 LFO1 Depth", 967, 75, false, 3, 6},
    {"Osc1 Wave Ctrl", 888, 165, false, 3, 5}, {"Osc1 PWM Depth", 967, 165, false, 3, 4},
    {"Osc2 Mix 1", 1208, 105, true, 4, 7}, {"Osc2 Mix 2", 1261, 105, true, 4, 5}, {"Osc2 Mix 3", 1314, 105, true, 4, 3},
    {"Osc2 LFO1 Depth", 1475, 75, false, 4, 6}, {"Osc2 Fine", 1553, 75, false, 4, 4},
    {"Osc2 Width", 1395, 165, false, 4, 0}, {"Osc2 PWM Depth", 1475, 165, false, 4, 1}, {"Osc2 Range", 1553, 165, false, 4, 2},
    {"Filter Frequency", 1689, 75, false, 5, 5}, {"Filter KBD Track", 1781, 75, false, 5, 7}, {"Filter Resonance", 1689, 165, false, 5, 3},
    {"Filter Attack", 1875, 105, true, 5, 6}, {"Filter Decay", 1928, 105, true, 5, 4},
    {"Filter Sustain", 1981, 105, true, 6, 5}, {"Filter Release", 2035, 105, true, 6, 7},
    {"Filter Env Depth", 2130, 75, false, 6, 6}, {"Filter LFO1 Depth", 2130, 165, false, 6, 4},
    {"Amp Attack", 2277, 105, true, 6, 2}, {"Amp Decay", 2330, 105, true, 6, 1},
    {"Amp Sustain", 2383, 105, true, 7, 5}, {"Amp Release", 2435, 105, true, 7, 7},
    {"Amp Amount", 2475, 75, false, 7, 6}, {"Amp LFO1 Depth", 2475, 165, false, 7, 4},
    {"LFO2 Rate", 72, 360, false, 1, 3}, {"LFO2 Depth", 152, 360, false, 1, 0}, {"Keyboard Time", 258, 360, false, 1, 1},
    {"Overdrive", 527, 487, false, 2, 3}, {"Chorus", 607, 487, false, 2, 0}, {"Delay", 687, 487, false, 2, 1}, {"Reverb", 767, 487, false, 2, 2},
    {"Arp Tempo", 874, 360, false, 3, 1}, {"Phrase Tempo", 1035, 360, false, 3, 2},
    {"EQ Low", 2104, 435, true, 6, 3}, {"EQ Freq", 2183, 435, true, 6, 0}, {"EQ B/C", 2237, 435, true, 7, 1},
    {"EQ Q", 2290, 435, true, 7, 0}, {"EQ High", 2368, 435, true, 7, 3}, {"Master Volume", 2480, 435, true, 7, 2},
};

static const PanelKey panel_keys[] = {
    {"LFO1 Wave", 113, 255, 1, 7}, {"Common Osc", 580, 255, 1, 4},
    {"Osc1 Wave 1", 703, 255, 1, 3}, {"Osc1 Wave 2", 755, 255, 1, 2}, {"Osc1 Wave 3", 807, 255, 1, 1}, {"Osc1 PWM Source", 967, 255, 1, 0},
    {"Osc2 Wave 1", 1208, 255, 5, 3}, {"Osc2 Wave 2", 1261, 255, 5, 4}, {"Osc2 Wave 3", 1314, 255, 5, 5},
    {"Sync", 1397, 75, 5, 7}, {"Osc2 PWM Source", 1475, 255, 5, 2}, {"Osc2 KBD Track", 1556, 255, 5, 1},
    {"Filter1", 1689, 255, 5, 0}, {"Routing", 1781, 165, 6, 6}, {"Filter2", 1781, 255, 5, 6}, {"Autopan", 2477, 255, 6, 7},
    {"LFO2 Wave", 72, 608, 3, 7}, {"LFO2 Depth Select", 152, 608, 3, 3},
    {"Portamento", 258, 484, 2, 4}, {"Unison", 339, 484, 2, 6}, {"Mono", 421, 359, 1, 6}, {"Legato", 421, 484, 2, 5},
    {"OCT-", 258, 608, 3, 4}, {"OCT+", 339, 608, 3, 6}, {"OSC SHIFT", 421, 608, 3, 5},
    {"Level/Parameter", 527, 359, 1, 5}, {"Overdrive On", 527, 608, 4, 5}, {"Chorus On", 607, 608, 4, 6}, {"Delay On", 687, 608, 4, 4}, {"Reverb On", 767, 608, 4, 3},
    {"Arp Hold", 874, 525, 2, 2}, {"Arp On", 874, 608, 4, 2}, {"Arp Mode", 953, 608, 4, 1}, {"Arp Range", 1033, 608, 4, 7},
    {"Phrase On", 953, 525, 2, 1}, {"Phrase Rec", 1034, 525, 2, 7},
    {"ESC", 1142, 504, 9, 0}, {"F1", 1238, 504, 9, 1}, {"F2", 1333, 504, 9, 2}, {"F3", 1428, 504, 9, 3}, {"F4", 1524, 504, 9, 4}, {"Disp Hold", 1620, 504, 9, 5},
    {"Split", 1195, 567, 10, 0}, {"Layer", 1246, 567, 10, 1}, {"Single", 1300, 567, 10, 2},
    {"Part 1", 1195, 608, 11, 0}, {"Part 2", 1246, 608, 11, 1}, {"Part 3", 1300, 608, 11, 2}, {"Part 4", 1354, 608, 11, 6},
    {"Program", 1462, 567, 10, 3}, {"Timbre", 1514, 567, 10, 4}, {"System", 1569, 567, 10, 5},
    {"Panel Scan", 1462, 608, 11, 3}, {"Routers", 1514, 608, 11, 4}, {"Write", 1569, 608, 11, 5},
    {"Up", 1755, 505, 9, 7}, {"Down", 1755, 567, 10, 6}, {"Left", 1724, 536, 9, 6}, {"Right", 1787, 536, 10, 7},
    {"7", 1889, 379, 8, 0}, {"8", 1941, 379, 6, 0}, {"9", 1994, 379, 7, 0},
    {"4", 1889, 421, 8, 1}, {"5", 1941, 421, 6, 1}, {"6", 1994, 421, 7, 1},
    {"1", 1889, 463, 8, 2}, {"2", 1941, 463, 6, 2}, {"3", 1994, 463, 7, 2},
    {"Bank", 1889, 505, 8, 3}, {"0", 1941, 505, 6, 3}, {"Enter", 1994, 505, 7, 3},
    {"Morph Auto", 1890, 567, 8, 4}, {"Morph Manual", 1941, 567, 6, 4}, {"Morph Enable", 1994, 567, 7, 4},
    {"Loc 1", 1890, 608, 8, 5}, {"Loc 2", 1941, 608, 6, 5}, {"Motion Rec", 1994, 608, 7, 5},
    {"EQ Mid Mode", 2237, 608, 7, 6}, {"EQ Bypass", 2369, 608, 7, 7},
};

static const float panel_dial_x = 1755, panel_dial_y = 380, panel_dial_r = 60;
static const float panel_lcd_x0 = 1190, panel_lcd_y0 = 325, panel_lcd_x1 = 1578, panel_lcd_y1 = 433;
