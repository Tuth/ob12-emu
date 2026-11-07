#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef min
#undef max
#include "emu.h"
#include "f2mc16.h"
#include "mb90611.h"
#include "tms57070/tms57070.h"
#include "ob12_engine.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <immintrin.h>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#pragma comment(lib, "synchronization.lib")

#define OB12_NO_MAIN
namespace master {
#include "ob12_master.cpp"
}
namespace slave {
#include "ob12_slave.cpp"
}

namespace {

// Workers spin through the gaps of a burst and sleep on the address between bursts.
struct Gate {
    alignas(64) std::atomic<long> go{0};
    alignas(64) std::atomic<int> left{0};
    alignas(64) std::atomic<int> sleepers{0};
    void open(int workers) {
        left.store(workers, std::memory_order_relaxed);
        go.fetch_add(1, std::memory_order_seq_cst);
        if (sleepers.load(std::memory_order_seq_cst)) WakeByAddressAll(&go);
    }
    void wait() { while (left.load(std::memory_order_acquire)) _mm_pause(); }
    void done() { left.fetch_sub(1, std::memory_order_release); }
};

static int spin_limit = getenv("OB12_SPIN") ? atoi(getenv("OB12_SPIN")) : 2000;

static void wait_for(Gate& gate, long seen, const std::atomic<bool>& stop) {
    for (int spins = 0; spins < spin_limit; spins++) {
        if (gate.go.load(std::memory_order_acquire) != seen) return;
        _mm_pause();
    }
    gate.sleepers.fetch_add(1, std::memory_order_seq_cst);
    while (gate.go.load(std::memory_order_seq_cst) == seen && !stop.load(std::memory_order_relaxed))
        WaitOnAddress(&gate.go, &seen, sizeof seen, 5);
    gate.sleepers.fetch_sub(1, std::memory_order_seq_cst);
}

// Host stream per chip: 0C header+PMEM, 05 CMEM, 03 then 5-byte address+value groups.
struct Host {
    tms::Core* dsp = nullptr;
    enum State { Idle, Program, Coefficients, Update } state = Idle;
    std::vector<u8> buffer;
    bool running = false, start_pending = false;

    // A coefficient word written right after the one below it (A then A+1, as the Slave writes a glide
    // stage's rate and pole) takes effect together with it, so no DSP sample sees one without the
    // other. A held word waits at most one half frame. Same as ob12_system; see "The Raver" in
    // docs/HARDWARE.md.
    bool pair_words = true;
    int held_address = -1, held_age = 0;
    int32_t held_value = 0;

    void word(int address, int32_t value) {
        if (!pair_words) { dsp->cmem[address] = value; return; }
        if (held_address >= 0) {
            int first = held_address;
            held_address = -1;
            dsp->cmem[first] = held_value;
            if (address == first + 1) { dsp->cmem[address] = value; return; }
        }
        held_address = address; held_value = value; held_age = 0;
    }
    void half_frame() {
        if (held_address >= 0 && ++held_age >= 2) { dsp->cmem[held_address] = held_value; held_address = -1; }
    }

    void deselect() { if (state == Update) { state = Idle; buffer.clear(); } }
    void byte(u8 data) {
        switch (state) {
        case Idle:
            if (data == 0x0c) { state = Program; buffer.clear(); }
            else if (data == 0x05) { state = Coefficients; buffer.clear(); }
            else if (data == 0x03) { state = Update; buffer.clear(); }
            break;
        case Program:
            buffer.push_back(data);
            if (buffer.size() == 12 + 2048) {
                const u8* h = buffer.data();
                running = false;
                dsp->reset();
                dsp->load_control(h);
                for (int i = 0; i < 512; i++) {
                    const u8* w = h + 12 + i * 4;
                    dsp->pmem[i] = w[0] << 24 | w[1] << 16 | w[2] << 8 | w[3];
                }
                state = Idle;
            }
            break;
        case Coefficients:
            buffer.push_back(data);
            if (buffer.size() == 1536) {
                for (int i = 0; i < 512; i++) {
                    const u8* w = buffer.data() + i * 3;
                    dsp->cmem[i] = (int32_t)((uint32_t)(w[0] << 16 | w[1] << 8 | w[2]) << 8) >> 8;
                }
                start_pending = true;
                state = Idle;
            }
            break;
        case Update:
            buffer.push_back(data);
            if (buffer.size() == 5) {
                int address = (buffer[0] << 8 | buffer[1]) & 0x1ff;
                word(address, (int32_t)((uint32_t)(buffer[2] << 16 | buffer[3] << 8 | buffer[4]) << 8) >> 8);
                buffer.clear();
            }
            break;
        }
    }
};

}  // namespace

struct Engine::Impl {
    master::Master* m = nullptr;
    slave::Slave* s = nullptr;
    Host host[5];
    // Chips read each other two half frames late, so two half frames can run per gate. Host bytes
    // stay per chip and per half, so each lands between the same two half frames as on one thread.
    struct Byte { u8 data; bool deselect; };
    std::vector<Byte> held[2][5][2];  // batch parity, chip, half
    int32_t snap[2][2][5][3][2] = {};  // batch parity, half, chip, port, channel: ports after that run
    int filling = 0, half = 0;         // where the CPUs put bytes now
    long batch = 0;
    size_t m_sent = 0, s_sent = 0;
    long k = 0;
    int slice = 167;  // CPU cycles in this half frame
    Gate cpu_gate, dsp_gate;
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    std::string error;
    bool one_cpu_thread = getenv("OB12_ONE_CPU") != nullptr;
    int32_t* out = nullptr;
    int made = 0;

    // Inputs from other threads, taken in at each millisecond.
    std::mutex input_lock;
    std::atomic<bool> input_waiting{false};
    std::vector<u8> midi_bytes;
    struct KeyEvent { int bar, bit; bool down; };
    std::vector<KeyEvent> key_events;
    struct PotEvent { int channel, mux, value; };
    std::vector<PotEvent> pot_events;
    int turns = 0, enc_detents = 0;
    long enc_start = 0;

    mutable std::mutex lcd_lock;
    uint8_t lcd_copy[30 * 64] = {};
    uint8_t led_copy[16] = {};
    bool lcd_ready = false;
    std::atomic<bool> ready{false};
    long ready_at = 0;  // half frame the Slave had answered

    void take_inputs() {
        if (!input_waiting.exchange(false)) return;
        std::lock_guard<std::mutex> hold(input_lock);
        if (!midi_bytes.empty()) { m->midi_in(midi_bytes); midi_bytes.clear(); }
        for (auto& e : key_events) {
            u8 bit = (u8)(1 << e.bit);
            if (e.down) m->key_live[e.bar] |= bit; else m->key_live[e.bar] &= ~bit;
        }
        key_events.clear();
        for (auto& e : pot_events) m->pot_live[e.channel][e.mux] = (short)e.value;
        pot_events.clear();
        enc_detents += turns;
        turns = 0;
    }

    void encoder_step() {
        if (!enc_detents) return;
        if (!enc_start) enc_start = k;
        int phase = (int)((k - enc_start) / 384);  // 4 ms per quarter
        static const int a_first[4][2] = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};
        int i = phase & 3;
        if (enc_detents > 0) m->enc_set(a_first[i][0], a_first[i][1]);
        else m->enc_set(a_first[i][1], a_first[i][0]);
        if (phase >= 4) {
            enc_detents += enc_detents > 0 ? -1 : 1;
            enc_start = enc_detents ? k : 0;
        }
    }

    void cpu_slice() {
        double now = k / 96000.0;
        m->pot_clock = now;
        encoder_step();
        if (k % 96 == 0) {
            s->ppg_tick();
            s->reload_tick();
            take_inputs();
            if (k % 3072 == 0) {
                std::lock_guard<std::mutex> hold(lcd_lock);
                lcd_ready = m->lcd.bitmap(lcd_copy);
                memcpy(led_copy, m->led, sizeof led_copy);
            }
        }
        slice = k % 3 == 2 ? 166 : 167;  // 16 MHz: 500 cycles per three half frames
        if (one_cpu_thread) {
            m->run(slice); m->tick(m->executed());
            s->run(slice); s->tick(s->executed());
        } else {
            cpu_gate.open(1);
            s->run(slice); s->tick(s->executed());
            cpu_gate.wait();
        }
        if (!error.empty()) throw std::runtime_error(error);
        auto& mu = m->link();
        auto& su = s->link();
        for (; m_sent < mu.head.size(); m_sent++) su.rx_queue.push_back(mu.head[m_sent]);
        for (; s_sent < su.head.size(); s_sent++) mu.rx_queue.push_back(su.head[s_sent]);
        if (s_sent >= 18 && !ready_at) { ready_at = k; ready.store(true, std::memory_order_relaxed); }
        k++;
    }

    // One chip through both half frames of a batch, on its own worker.
    void chip_batch(int n) {
        long b = dsp_batch;
        int now = b & 1, before = now ^ 1;
        Host& h = host[n];
        tms::Core* d = h.dsp;
        for (int part = 0; part < 2; part++) {
            auto& list = held[now][n][part];
            for (auto& byte : list) {
                if (byte.deselect) h.deselect(); else h.byte(byte.data);
            }
            list.clear();
            // A word sent in one slot arrives two slots later, see "Two-slot audio bus".
            for (int c = 0; c < 2; c++) {
                if (n == 0) {
                    d->ar[0][c] = snap[before][part][4][0][c];
                    d->ar[1][c] = 0;
                } else {
                    d->ar[0][c] = snap[before][part][n - 1][1][c];
                    d->ar[1][c] = snap[before][part][n - 1][2][c];
                }
            }
            if (h.start_pending && part == 0) { h.start_pending = false; h.running = true; }
            h.half_frame();
            if (h.running) for (int i = 0; i < 256; i++) d->step();
            memcpy(snap[now][part][n], d->ax, sizeof d->ax);
        }
    }

    long dsp_batch = 0;

    void next_batch() {
        filling = batch & 1;
        half = 0;
        cpu_slice();
        half = 1;
        cpu_slice();
        if (batch > 0) {
            dsp_gate.wait();
            const int32_t* frame = snap[(batch - 1) & 1][1][4][1];  // MOUT after the odd half frame
            out[made * 2] = frame[0];
            out[made * 2 + 1] = frame[1];
            made++;
        }
        dsp_batch = batch;
        dsp_gate.open(5);
        batch++;
    }
};

Engine::Engine(const std::string& rom_dir) : impl(new Impl) {
    Impl& e = *impl;
    tms::Options options;
    options.audio_port_writes = true;
    options.true_dmem_ring = true;
    options.lock_delay = true;
    options.maclo_multiply = true;
    options.divide_1fc = true;
    options.noise_generator = true;
    options.late_mode_change = true;
    options.compare_3e = true;
    options.maclo_shift = true;
    options.maclo_unsigned = true;
    options.queued_pointer_load = true;
    options.bus_sum_saturates = true;
    options.reuse_coefficient = true;
    options.late_saturation_mode = true;
    options.xread_flush = true;

    e.m = new master::Master();
    e.s = new slave::Slave();
    e.m->load(rom_dir);
    e.s->load(rom_dir);
    e.m->attach(e.m);
    e.s->attach(e.s);
    e.m->reset();
    e.s->reset();
    e.m->lean = e.s->lean = true;
    // DASP4 has 3 x 44C256 DRAM, 256K x 12 bits: 128K words of 24 bits in two accesses.
    tms::Options options4 = options;
    options4.xmem_words = 0x20000;
    for (int n = 0; n < 5; n++) e.host[n].dsp = new tms::Core(n == 4 ? options4 : options);
    e.s->on_dsp_byte = [&e](u8 selected, u8 data) {
        for (int chip = 0; chip < 5; chip++) if (selected & (1 << chip)) e.held[e.filling][chip][e.half].push_back({data, false});
    };
    e.s->on_dsp_select = [&e]() {
        for (int chip = 0; chip < 5; chip++) e.held[e.filling][chip][e.half].push_back({0, true});
    };

    e.workers.emplace_back([&e]() {
        long seen = 0;
        for (;;) {
            wait_for(e.cpu_gate, seen, e.stop);
            if (e.stop) return;
            seen++;
            try { e.m->run(e.slice); e.m->tick(e.m->executed()); }
            catch (const std::exception& error) { e.error = error.what(); }
            e.cpu_gate.done();
        }
    });
    for (int n = 0; n < 5; n++) e.workers.emplace_back([&e, n]() {
        long seen = 0;
        for (;;) {
            wait_for(e.dsp_gate, seen, e.stop);
            if (e.stop) return;
            seen++;
            e.chip_batch(n);
            e.dsp_gate.done();
        }
    });
}

Engine::~Engine() {
    if (getenv("OB12_CLOCK_LOG")) printf("clock: %.6f s of sound, Master %.6f s, Slave %.6f s at 16 MHz, ready at %.4f s\n", impl->k / 96000.0, impl->m->cycles_total / 16e6, impl->s->cycles_total / 16e6, impl->ready_at / 96000.0);
    impl->stop = true;
    for (auto& t : impl->workers) t.join();
    delete impl->m;
    delete impl->s;
    for (auto& h : impl->host) delete h.dsp;
    delete impl;
}

void Engine::run(int32_t* frames, int n) {
    Impl& e = *impl;
    e.out = frames;
    e.made = 0;
    while (e.made < n) e.next_batch();
}

void Engine::midi(const uint8_t* bytes, int count) {
    std::lock_guard<std::mutex> hold(impl->input_lock);
    impl->midi_bytes.insert(impl->midi_bytes.end(), bytes, bytes + count);
    impl->input_waiting = true;
}

void Engine::set_pot(int channel, int mux, int value) {
    std::lock_guard<std::mutex> hold(impl->input_lock);
    impl->pot_events.push_back({channel, mux, value});
    impl->input_waiting = true;
}

void Engine::key(int bar, int bit, bool down) {
    std::lock_guard<std::mutex> hold(impl->input_lock);
    impl->key_events.push_back({bar, bit, down});
    impl->input_waiting = true;
}

void Engine::turn(int detents) {
    std::lock_guard<std::mutex> hold(impl->input_lock);
    impl->turns += detents;
    impl->input_waiting = true;
}

bool Engine::sounding_ready() const { return impl->ready.load(std::memory_order_relaxed); }

void Engine::leds(uint8_t* latches) const {
    std::lock_guard<std::mutex> hold(impl->lcd_lock);
    memcpy(latches, impl->led_copy, sizeof impl->led_copy);
}

bool Engine::lcd(uint8_t* bitmap) const {
    std::lock_guard<std::mutex> hold(impl->lcd_lock);
    if (!impl->lcd_ready) return false;
    memcpy(bitmap, impl->lcd_copy, sizeof impl->lcd_copy);
    return true;
}
