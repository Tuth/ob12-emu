// Peripherals shared by the OB-12 Master and Slave MB90611A models.
#pragma once
#include <deque>
#include "emu.h"
#include <vector>

struct ReloadTimer {
    int base = 0;
    int vector = 0;
    u16 tmcsr = 0, reload = 0, count = 0;
    s64 prescale = 0;
    bool running = false;
    int divider() const { int csl = (tmcsr >> 10) & 3; return csl == 0 ? 2 : csl == 1 ? 8 : csl == 2 ? 32 : 0; }
    void trigger() { count = reload; running = true; prescale = 0; }
    void advance(int cycles) {
        if (!running || !(tmcsr & 2) || !divider()) return;
        prescale += cycles;
        int div = divider();
        while (prescale >= div) {
            prescale -= div;
            if (count == 0) {
                tmcsr |= 4;  // UF
                if (tmcsr & 0x10) count = reload; else { running = false; return; }
            } else count--;
        }
    }
    bool pending() const { return (tmcsr & 0x0c) == 0x0c; }
};

struct Uart {
    int base = 0, tx_vector = 0, rx_vector = 0;
    const char* name = "";
    u8 scr = 0, smr = 0, ssr = 0x08;  // TDRE set
    int tx_timer = 0, rx_timer = 0;
    long sent = 0;
    u8 rx_data = 0;
    std::deque<u8> rx_queue;  // a whole .syx can wait here: drained from the front
    std::vector<u8> head;
    void advance(int cycles) {
        if (tx_timer > 0 && (tx_timer -= cycles) <= 0) { tx_timer = 0; ssr |= 0x08; }
        if (!rx_queue.empty() && (rx_timer -= cycles) <= 0 && !(ssr & 0x10)) {
            rx_data = rx_queue.front();
            rx_queue.erase(rx_queue.begin());
            ssr |= 0x10;  // RDRF
            rx_timer = 5120;  // one MIDI byte at 31250 baud
        }
    }
    u8 read() { ssr &= ~0x10; return rx_data; }
    void write(u8 data) {
        sent++;
        head.push_back(data);
        ssr &= ~0x08;
        tx_timer = 2000;
    }
    bool tx_pending() const { return (ssr & 0x08) && (ssr & 0x01); }
    bool rx_pending() const { return (ssr & 0x10) && (ssr & 0x02); }
};

