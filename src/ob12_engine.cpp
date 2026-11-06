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

#define OB12_NO_MAIN
namespace master {
#include "ob12_master.cpp"
}
namespace slave {
#include "ob12_slave.cpp"
}

namespace {

struct Gate {
    alignas(64) std::atomic<long> go{0};
    alignas(64) std::atomic<int> left{0};
    void open(int workers) { left.store(workers, std::memory_order_relaxed); go.fetch_add(1, std::memory_order_release); }
    void wait() { for (long spins = 0; left.load(std::memory_order_acquire); spins++) { if (spins < 20000) _mm_pause(); else std::this_thread::yield(); } }
    void done() { left.fetch_sub(1, std::memory_order_release); }
};

// Spins while the machine is running, gives the core back when it is held up.
static void wait_for(Gate& gate, long seen, const std::atomic<bool>& stop) {
    long spins = 0;
    while (gate.go.load(std::memory_order_acquire) == seen) {
        if (stop.load(std::memory_order_relaxed)) return;
        if (++spins < 20000) _mm_pause();
        else if (spins < 2000000) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

// Host stream per chip: 0C header+PMEM, 05 CMEM, 03 then 5-byte address+value groups.
struct Host {
    tms::Core* dsp = nullptr;
    enum State { Idle, Program, Coefficients, Update } state = Idle;
    std::vector<u8> buffer;
    bool running = false, start_pending = false;

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
                dsp->cmem[address] = (int32_t)((uint32_t)(buffer[2] << 16 | buffer[3] << 8 | buffer[4]) << 8) >> 8;
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
    std::vector<std::pair<u8, u8>> held;
    size_t m_sent = 0, s_sent = 0;
    int32_t wire[4][5][3][2] = {};
    long k = 0;
    Gate cpu_gate, dsp_gate;
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    std::string error;
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
    bool lcd_ready = false;
    std::atomic<bool> ready{false};

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

    void post(long frame_k) {
        if (!(frame_k & 1)) return;
        auto* d = host[4].dsp;
        out[made * 2] = d->ax[1][0];
        out[made * 2 + 1] = d->ax[1][1];
        made++;
    }

    void half_frame() {
        double now = k / 96000.0;
        m->pot_clock = now;
        encoder_step();
        if (k % 96 == 0) {
            s->ppg_tick();
            take_inputs();
            if (k % 3072 == 0) {
                std::lock_guard<std::mutex> hold(lcd_lock);
                lcd_ready = m->lcd.bitmap(lcd_copy);
            }
        }
        const int slice = 167;
        cpu_gate.open(1);
        s->run(slice); s->tick(slice - s->cycles_left());
        cpu_gate.wait();
        if (!error.empty()) throw std::runtime_error(error);
        if (k > 0) {
            dsp_gate.wait();
            post(k - 1);
        }
        for (auto& b : held) {
            if (b.first == 0x80) { for (auto& h : host) h.deselect(); }
            else for (int chip = 0; chip < 5; chip++) if (b.first & (1 << chip)) host[chip].byte(b.second);
        }
        held.clear();
        auto& mu = m->link();
        auto& su = s->link();
        for (; m_sent < mu.head.size(); m_sent++) su.rx_queue.push_back(mu.head[m_sent]);
        for (; s_sent < su.head.size(); s_sent++) mu.rx_queue.push_back(su.head[s_sent]);
        if (s_sent >= 18) ready.store(true, std::memory_order_relaxed);

        // A word sent in one slot arrives two slots later, see "Two-slot audio bus".
        int32_t ax2[5][2], ax3[5][2];
        for (int n = 0; n < 5; n++) for (int c = 0; c < 2; c++) {
            ax2[n][c] = host[n].dsp->ax[1][c];
            ax3[n][c] = host[n].dsp->ax[2][c];
        }
        int slot = k % 4, from = (k + 3) % 4;
        for (int n = 0; n < 5; n++) for (int c = 0; c < 2; c++) {
            wire[slot][n][0][c] = host[n].dsp->ax[0][c];
            wire[slot][n][1][c] = ax2[n][c];
            wire[slot][n][2][c] = ax3[n][c];
        }
        for (int c = 0; c < 2; c++) {
            host[0].dsp->ar[0][c] = wire[from][4][0][c];
            host[0].dsp->ar[1][c] = 0;
            for (int n = 1; n < 5; n++) {
                host[n].dsp->ar[0][c] = wire[from][n - 1][1][c];
                host[n].dsp->ar[1][c] = wire[from][n - 1][2][c];
            }
        }
        for (int n = 0; n < 5; n++) {
            Host& h = host[n];
            if (h.start_pending && (k & 1) == 0) { h.start_pending = false; h.running = true; }
        }
        dsp_gate.open(5);
        k++;
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

    e.m = new master::Master();
    e.s = new slave::Slave();
    e.m->load(rom_dir);
    e.s->load(rom_dir);
    e.m->attach(e.m);
    e.s->attach(e.s);
    e.m->reset();
    e.s->reset();
    e.m->lean = e.s->lean = true;
    for (auto& h : e.host) h.dsp = new tms::Core(options);
    e.s->on_dsp_byte = [&e](u8 selected, u8 data) { e.held.push_back({selected, data}); };
    e.s->on_dsp_select = [&e]() { e.held.push_back({0x80, 0}); };

    e.workers.emplace_back([&e]() {
        long seen = 0;
        for (;;) {
            wait_for(e.cpu_gate, seen, e.stop);
            if (e.stop) return;
            seen++;
            try { e.m->run(167); e.m->tick(167 - e.m->cycles_left()); }
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
            if (e.host[n].running) for (int i = 0; i < 256; i++) e.host[n].dsp->step();
            e.dsp_gate.done();
        }
    });
}

Engine::~Engine() {
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
    while (e.made < n) e.half_frame();
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

bool Engine::lcd(uint8_t* bitmap) const {
    std::lock_guard<std::mutex> hold(impl->lcd_lock);
    if (!impl->lcd_ready) return false;
    memcpy(bitmap, impl->lcd_copy, sizeof impl->lcd_copy);
    return true;
}
