// Whole instrument: Master and Slave CPUs linked over UART2, five TMS57070 cores driven by
// the Slave's host bytes, DSP chain wired per service manual sheets 09/15 and 10/15.
//
// usage: ob12_system <rom dir> <seconds> <note seconds> <out.wav>
//   OB12_DIRECT=n    send one note per channel 0..n-1 straight to the Slave, 1 s apart
//   OB12_MIDI=1      send the note to the Master's MIDI input instead
//   OB12_PHASE=0|1   start DASP4 on the even or odd half frame (default 1)
//   OB12_NO_1FC=1    disable the 1FC divide step
//   OB12_NOISE=1     enable the noise secondary, 0x34
//   OB12_POT_SWEEP=t move all pots from t seconds on
//   OB12_FORCE_GAIN=1  diagnostic: force DASP4 output gains C00D, C00F, C0CB
#include "emu.h"
#include "f2mc16.h"
#include "mb90611.h"
#include "tms57070/tms57070.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <unordered_map>

#define OB12_NO_MAIN
namespace master {
#include "ob12_master.cpp"
}
namespace slave {
#include "ob12_slave.cpp"
}

// Host stream per chip: 0C header+PMEM, 05 CMEM, 03 then 5-byte address+value groups.
struct Host {
    tms::Core* dsp = nullptr;
    enum State { Idle, Program, Coefficients, Update } state = Idle;
    std::vector<u8> buffer;
    bool running = false, start_pending = false;
    long updates = 0, programs = 0, unknown = 0;

    void deselect() { if (state == Update) { state = Idle; buffer.clear(); } }
    void byte(u8 data) {
        switch (state) {
        case Idle:
            if (data == 0x0c) { state = Program; buffer.clear(); }
            else if (data == 0x05) { state = Coefficients; buffer.clear(); }
            else if (data == 0x03) { state = Update; buffer.clear(); }
            else if (data != 0x00) unknown++;
            break;
        case Program:
            buffer.push_back(data);
            if (buffer.size() == 12 + 2048) {
                const u8* h = buffer.data();
                running = false;
                dsp->reset();  // keeps DCIRC, DIR and CIR set by the start-up program
                dsp->load_control(h);
                for (int i = 0; i < 512; i++) {
                    const u8* w = h + 12 + i * 4;
                    dsp->pmem[i] = w[0] << 24 | w[1] << 16 | w[2] << 8 | w[3];
                }
                programs++;
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
                updates++;
                buffer.clear();
            }
            break;
        }
    }
};

static void write_wav(const std::string& path, const std::vector<int16_t>& samples) {
    std::ofstream f(path, std::ios::binary);
    auto u32le = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto u16le = [&](uint16_t v) { f.write((const char*)&v, 2); };
    uint32_t bytes = (uint32_t)samples.size() * 2;
    f.write("RIFF", 4); u32le(36 + bytes); f.write("WAVEfmt ", 8); u32le(16); u16le(1); u16le(2);
    u32le(48000); u32le(48000 * 4); u16le(4); u16le(16);
    f.write("data", 4); u32le(bytes);
    f.write((const char*)samples.data(), bytes);
}

int main(int argc, char** argv) {
    if (argc < 5) { puts("usage: ob12_system <rom dir> <seconds> <note seconds> <out.wav>"); return 2; }
    std::string rom = argv[1];
    double seconds = atof(argv[2]), note_at = atof(argv[3]);
    std::string wav = argv[4];
    int direct = getenv("OB12_DIRECT") ? atoi(getenv("OB12_DIRECT")) : 0;
    bool midi = getenv("OB12_MIDI") != nullptr;
    int phase = getenv("OB12_PHASE") ? atoi(getenv("OB12_PHASE")) : 1;
    bool force_gain = getenv("OB12_FORCE_GAIN") != nullptr;

    tms::Options options;
    options.audio_port_writes = true;
    options.true_dmem_ring = true;
    options.lock_delay = true;
    options.maclo_multiply = true;
    options.divide_1fc = getenv("OB12_NO_1FC") == nullptr;
    options.noise_generator = getenv("OB12_NOISE") != nullptr;

    auto* m = new master::Master();
    if (getenv("OB12_POT_SWEEP")) m->pot_sweep_from = atof(getenv("OB12_POT_SWEEP"));
    auto* s = new slave::Slave();
    m->load(rom);
    s->load(rom);
    m->attach(m); s->attach(s);
    m->reset(); s->reset();

    Host host[5];
    for (auto& h : host) h.dsp = new tms::Core(options);
    s->on_dsp_byte = [&](u8 selected, u8 data) {
        for (int chip = 0; chip < 5; chip++) if (selected & (1 << chip)) host[chip].byte(data);
    };
    s->on_dsp_select = [&]() { for (auto& h : host) h.deselect(); };

    size_t m_sent = 0, s_sent = 0;
    std::vector<int16_t> out;
    std::vector<int32_t> bus;
    double sum[4] = {0, 0, 0, 0}, chip_sum[5] = {0, 0, 0, 0, 0};
    long count = 0;
    long total = (long)(seconds * 96000);
    int sent_notes = 0;
    int midi_channel = 0;
    try {
        for (long k = 0; k < total; k++) {
            double now = k / 96000.0;
            // One note per MIDI channel, half a second apart, into the Master's MIDI input.
            if (midi && midi_channel < 16 && now >= note_at + midi_channel * 0.5) {
                m->midi_in({(u8)(0x90 | midi_channel), 0x3c, 0x64});
                midi_channel++;
            }
            if (sent_notes < direct && now >= note_at + sent_notes) {
                s->link().rx_queue.insert(s->link().rx_queue.end(), {(u8)(0x90 | sent_notes), 0x3c, 0x64});
                sent_notes++;
            }
            const int slice = 167;
            m->run(slice); m->tick(slice - m->cycles_left());
            s->run(slice); s->tick(slice - s->cycles_left());
            auto& mu = m->link();
            auto& su = s->link();
            for (; m_sent < mu.head.size(); m_sent++) su.rx_queue.push_back(mu.head[m_sent]);
            for (; s_sent < su.head.size(); s_sent++) mu.rx_queue.push_back(su.head[s_sent]);

            // Snapshot the serial links, so every chip sees the previous half frame.
            int32_t ax2[5][2], ax3[5][2];
            for (int n = 0; n < 5; n++) for (int c = 0; c < 2; c++) {
                ax2[n][c] = host[n].dsp->ax[1][c];
                ax3[n][c] = host[n].dsp->ax[2][c];
            }
            for (int c = 0; c < 2; c++) {
                host[0].dsp->ar[0][c] = host[4].dsp->ax[0][c];  // LFO return
                host[0].dsp->ar[1][c] = 0;
                for (int n = 1; n < 5; n++) {
                    host[n].dsp->ar[0][c] = ax2[n - 1][c];
                    host[n].dsp->ar[1][c] = ax3[n - 1][c];
                }
            }
            // Diagnostic only: DASP4 output gains the firmware has not been seen to write.
            if (force_gain) for (int index : {0x0d, 0x0f, 0xcb}) host[4].dsp->cmem[index] = 0x200000;
            for (int n = 0; n < 5; n++) {
                Host& h = host[n];
                if (h.start_pending && ((k + (n == 4 ? phase : 0)) & 1) == 0) { h.start_pending = false; h.running = true; }
                if (h.running) for (int i = 0; i < 256; i++) h.dsp->step();
            }
            if (k & 1) {
                auto* d = host[4].dsp;
                int32_t v[4] = {d->ax[1][0], d->ax[1][1], d->ax[2][0], d->ax[2][1]};
                for (int c = 0; c < 4; c++) sum[c] += (double)v[c] * v[c];
                for (int n = 0; n < 5; n++) {
                    double l = host[n].dsp->ax[1][0], r = host[n].dsp->ax[1][1];
                    chip_sum[n] += l * l + r * r;
                }
                out.push_back((int16_t)(v[0] >> 8));
                out.push_back((int16_t)(v[1] >> 8));
                count++;
            }
            bus.push_back(host[4].dsp->ar[0][0]);
            bus.push_back(host[4].dsp->ar[0][1]);
            if (k % 96000 == 95999) {
                printf("t=%.0f M>S %zu S>M %zu host", (k + 1) / 96000.0, m_sent, s_sent);
                for (auto& h : host) printf(" %ld/%ld%s", h.programs, h.updates, h.running ? "r" : "");
                printf("; AX2 rms");
                for (int n = 0; n < 5; n++) { printf(" %.0f", count ? sqrt(chip_sum[n] / count / 2) : 0); chip_sum[n] = 0; }
                printf("; MOUT %.0f %.0f AOUT %.0f %.0f\n", sqrt(sum[0] / count), sqrt(sum[1] / count), sqrt(sum[2] / count), sqrt(sum[3] / count));
                fflush(stdout);
                sum[0] = sum[1] = sum[2] = sum[3] = 0;
                count = 0;
            }
        }
    } catch (const std::exception& error) {
        printf("STOP: %s\n", error.what());
    }
    write_wav(wav, out);
    {
        std::ofstream link("build/live_master_to_slave.bin", std::ios::binary);
        link.write((const char*)m->link().head.data(), m->link().head.size());
    }
    printf("DASP4 C00D %06X C00E %06X C00F %06X C010 %06X C077 %06X C0CB %06X\n", host[4].dsp->cmem[0xd] & 0xffffff, host[4].dsp->cmem[0xe] & 0xffffff,
           host[4].dsp->cmem[0xf] & 0xffffff, host[4].dsp->cmem[0x10] & 0xffffff, host[4].dsp->cmem[0x77] & 0xffffff, host[4].dsp->cmem[0xcb] & 0xffffff);
    std::ofstream raw("build/voice_bus.raw", std::ios::binary);
    raw.write((const char*)bus.data(), bus.size() * 4);
    for (int n = 0; n < 5; n++) {
        printf("DASP%d DCIRC %X DOFF %X DIR %X %X CIR %X %X unknown host %ld, unimplemented:", n, host[n].dsp->dcirc, host[n].dsp->doff,
               host[n].dsp->dir[0], host[n].dsp->dir[1], host[n].dsp->cir[0], host[n].dsp->cir[1], host[n].unknown);
        for (auto& entry : host[n].dsp->unknown) printf(" %08X x%ld", entry.first, entry.second);
        puts("");
    }
    m->lcd.save_pbm("build/lcd.pbm");
    m->save_hot("build/hot.txt");
    m->save_ram("build/ram.bin");
    return 0;
}
