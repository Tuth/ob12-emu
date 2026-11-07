// The instrument as a library: two CPUs and five DSPs behind a small live interface.
// One thread calls run(). Any thread may call the input functions; they take effect at the next
// millisecond. ob12_system.cpp stays the research tool; this is what the player and plugin use.
#pragma once
#include <cstdint>
#include <string>

class Engine {
public:
    explicit Engine(const std::string& rom_dir);
    ~Engine();

    // Runs the machine until n frames at 48 kHz are out. 24 bit values, left and right interleaved.
    void run(int32_t* frames, int n);

    void midi(const uint8_t* bytes, int count);          // into the Master's MIDI input
    void set_pot(int channel, int mux, int value);       // ADC channel 0-7, mux 0-7, value 0-255
    void key(int bar, int bit, bool down);               // button bar 0-11, BUT bit 0-7
    void turn(int detents);                              // data encoder

    bool sounding_ready() const;                         // the Slave has reported in
    // 240 x 64, one bit per pixel, 30 bytes a row. False until the Master has set up the LCD.
    bool lcd(uint8_t* bitmap) const;
    // The 16 LED latch bytes the Master last wrote (CS2, 68FF80-8F).
    void leds(uint8_t* latches) const;

private:
    struct Impl;
    Impl* impl;
};
