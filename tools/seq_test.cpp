// A timed event list through the engine, MOUT to a WAV.
// usage: seq_test <rom dir> <seconds> <out.wav> event...
// events: t:on:nn  t:off:nn  (note hex, velocity 64)  t:enc:detents  t:pot:channel:mux:value(hex)
#include "ob12_engine.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

struct Event { long frame; int kind, a, b, c; };  // kind 0 on, 1 off, 2 encoder, 3 pot

int main(int argc, char** argv) {
    Engine e(argv[1]);
    double seconds = atof(argv[2]);
    std::vector<Event> events;
    for (int i = 4; i < argc; i++) {
        double t; char kind[8]; int a = 0, b = 0, c = 0;
        if (sscanf(argv[i], "%lf:%7[a-z]:%x:%d:%x", &t, kind, &a, &b, &c) < 3) continue;
        Event ev{(long)(t * 48000), 0, a, b, c};
        if (!strcmp(kind, "off")) ev.kind = 1;
        else if (!strcmp(kind, "enc")) { ev.kind = 2; sscanf(argv[i], "%lf:%7[a-z]:%d", &t, kind, &ev.a); }
        else if (!strcmp(kind, "pot")) { ev.kind = 3; sscanf(argv[i], "%lf:%7[a-z]:%d:%d:%x", &t, kind, &ev.a, &ev.b, &ev.c); }
        events.push_back(ev);
    }
    std::sort(events.begin(), events.end(), [](const Event& x, const Event& y) { return x.frame < y.frame; });
    long frames = (long)(seconds * 48000), done = 0;
    std::vector<int32_t> audio(frames * 2 + 960);
    size_t next = 0;
    while (done < frames) {
        while (next < events.size() && events[next].frame <= done) {
            const Event& ev = events[next++];
            if (ev.kind == 0) { uint8_t m[3] = {0x90, (uint8_t)ev.a, 0x64}; e.midi(m, 3); }
            else if (ev.kind == 1) { uint8_t m[3] = {0x80, (uint8_t)ev.a, 0x40}; e.midi(m, 3); }
            else if (ev.kind == 2) e.turn(ev.a);
            else e.set_pot(ev.a, ev.b, ev.c);
        }
        int n = (int)std::min<long>(240, frames - done);
        e.run(audio.data() + done * 2, n);
        done += n;
    }
    if (const char* path = getenv("SEQ_LCD")) {  // the display at the end, 240 x 64 PBM
        uint8_t bitmap[30 * 64];
        if (e.lcd(bitmap)) { std::ofstream p(path, std::ios::binary); p << "P4\n240 64\n"; p.write((const char*)bitmap, sizeof bitmap); }
    }
    std::ofstream f(argv[3], std::ios::binary);
    auto u32le = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto u16le = [&](uint16_t v) { f.write((const char*)&v, 2); };
    uint32_t bytes = (uint32_t)(frames * 4);
    f.write("RIFF", 4); u32le(36 + bytes); f.write("WAVEfmt ", 8); u32le(16); u16le(1); u16le(2);
    u32le(48000); u32le(48000 * 4); u16le(4); u16le(16);
    f.write("data", 4); u32le(bytes);
    for (long i = 0; i < frames * 2; i++) u16le((uint16_t)(int16_t)(audio[i] >> 8));
    return 0;
}
