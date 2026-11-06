// OB-12 Master CPU boot harness.
// Memory map from service manual sheets 03/15 and 04/15; peripherals from the MB90611A
// data sheet. Every I/O register has storage and every access is recorded: an unmapped
// register is a gap on the record, never a neutral read.
#include "emu.h"
#include "f2mc16.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <unordered_map>

static std::vector<u8> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + path);
    return std::vector<u8>((std::istreambuf_iterator<char>(file)), {});
}

struct Stat { long reads = 0, writes = 0; u32 first_pc = 0; u8 last = 0; };

// Toshiba T6963C, the controller named in the MGLS24064 data sheet.
class T6963 {
public:
    T6963() : vram(0x10000, 0) {}
    u8 status() const { return 0x0f; }
    u8 read_data() { u8 value = vram[pointer]; if (auto_read) pointer++; return value; }
    void write_data(u8 value) {
        if (auto_write) { vram[pointer++] = value; writes++; return; }
        args[0] = args[1];
        args[1] = value;
    }
    void command(u8 code) {
        commands[code]++;
        u16 value = args[0] | args[1] << 8;
        switch (code) {
        case 0x24: pointer = value; break;
        case 0x40: text_home = value; break;
        case 0x41: text_area = args[0]; break;
        case 0x42: graphic_home = value; break;
        case 0x43: graphic_area = args[0]; break;
        case 0xb0: auto_write = true; break;
        case 0xb1: auto_read = true; break;
        case 0xb2: auto_write = auto_read = false; break;
        case 0xc0: vram[pointer++] = args[1]; writes++; break;
        case 0xc2: vram[pointer--] = args[1]; writes++; break;
        case 0xc4: vram[pointer] = args[1]; writes++; break;
        case 0xc1: pointer++; break;
        case 0xc3: pointer--; break;
        default:
            if ((code & 0xf0) == 0x80) mode = code;
            if ((code & 0xf0) == 0x90) display = code;
            if ((code & 0xf0) == 0xf0) {
                u8 bit = 1 << (code & 7);
                if (code & 8) vram[pointer] |= bit; else vram[pointer] &= ~bit;
                writes++;
            }
            break;
        }
    }
    void report() const {
        printf("LCD: %ld writes, mode %02X display %02X text home %04X area %d graphic home %04X area %d\n",
               writes, mode, display, text_home, text_area, graphic_home, graphic_area);
        printf("LCD commands:");
        for (auto& entry : commands) printf(" %02X x%ld", entry.first, entry.second);
        puts("");
        if (text_area) {
            for (int row = 0; row < 8; row++) {
                printf("| ");
                for (int column = 0; column < text_area && column < 40; column++) {
                    u8 code = vram[(u16)(text_home + row * text_area + column)];
                    char c = code + 0x20;
                    putchar(c >= 32 && c < 127 ? c : '.');
                }
                puts(" |");
            }
        }
    }
    void save_pbm(const std::string& path) const {
        if (!graphic_area) return;
        std::ofstream file(path, std::ios::binary);
        file << "P4\n" << graphic_area * 8 << " 64\n";
        for (int row = 0; row < 64; row++)
            file.write((const char*)&vram[(u16)(graphic_home + row * graphic_area)], graphic_area);
    }
    long writes = 0;

private:
    std::vector<u8> vram;
    u8 args[2] = {0, 0};
    u16 pointer = 0, text_home = 0, graphic_home = 0;
    int text_area = 0, graphic_area = 0;
    u8 mode = 0, display = 0;
    bool auto_write = false, auto_read = false;
    std::map<int, long> commands;
};

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
    int tx_timer = 0;
    long sent = 0;
    std::vector<u8> head;
    void advance(int cycles) {
        if (tx_timer > 0 && (tx_timer -= cycles) <= 0) { tx_timer = 0; ssr |= 0x08; }
    }
    void write(u8 data) {
        sent++;
        if (head.size() < 48) head.push_back(data);
        ssr &= ~0x08;
        tx_timer = 2000;
    }
    bool tx_pending() const { return (ssr & 0x08) && (ssr & 0x01); }
    bool rx_pending() const { return (ssr & 0x10) && (ssr & 0x02); }
};

class Master : public f2mc16_device, public address_space {
public:
    Master() : f2mc16_device(machine_config(), nullptr, nullptr, 16000000) {
        flash.assign(0x100000, 0xff);
        ram.assign(0x40000, 0);
        memset(internal, 0, sizeof internal);
        memset(io, 0, sizeof io);
        io[0xa1] = 0xfb;  // CKSCR reset value
        for (int i = 0xb0; i < 0xc0; i++) io[i] = 7;  // ICR: interrupts disabled
        timer[0].base = 0x38; timer[0].vector = 0x1d;
        timer[1].base = 0x3c; timer[1].vector = 0x1e;
        uart[0].base = 0x20; uart[0].tx_vector = 0x27; uart[0].rx_vector = 0x18; uart[0].name = "UART0 LED";
        uart[1].base = 0x24; uart[1].tx_vector = 0x25; uart[1].rx_vector = 0x1a; uart[1].name = "UART1 MIDI";
        uart[2].base = 0x44; uart[2].tx_vector = 0x23; uart[2].rx_vector = 0x21; uart[2].name = "UART2 Slave";
    }
    void load(const std::string& rom) {
        place(rom + "/12cpu152-CPU-v1.52-FINAL.bin", 0x00000);
        place(rom + "/system-parameters.bin", 0xfa000);
        place(rom + "/boot_152-emergency-boot.bin", 0xfc000);
    }

    u8 read_byte(u32 address) override {
        address &= 0xffffff;
        if (address < 0x100) {
            Stat& s = touch(iostat, address);
            s.reads++;
            return io_read(address);
        }
        if (address < 0x500) return internal[address];
        if (address >= 0xf00000) return flash[address - 0xf00000];
        if (address >= 0xe00000) return ram[address & 0x3ffff];
        if (address == 0x68ff00) return lcd.read_data();
        if (address == 0x68ff01) return lcd.status();
        Stat& s = touch(ext, address & 0xfffff0);
        s.reads++;
        return 0xff;
    }
    void write_byte(u32 address, u8 data) override {
        address &= 0xffffff;
        if (address < 0x100) {
            Stat& s = touch(iostat, address);
            s.writes++;
            s.last = data;
            io_write(address, data);
            return;
        }
        if (address < 0x500) { internal[address] = data; return; }
        if (address >= 0xf00000) { Stat& s = touch(flashw, address & 0xffff00); s.writes++; s.last = data; return; }
        if (address >= 0xe00000) { ram[address & 0x3ffff] = data; return; }
        if (address == 0x68ff00) { lcd.write_data(data); return; }
        if (address == 0x68ff01) { lcd.command(data); return; }
        Stat& s = touch(ext, address & 0xfffff0);
        s.writes++;
        s.last = data;
    }

    void tick(int cycles) {
        cycles_total += cycles;
        for (auto& t : timer) t.advance(cycles);
        for (auto& u : uart) u.advance(cycles);
        // Time-base timer: counts the 4 MHz oscillator, 4 machine cycles per count.
        static const int shift[4] = {12, 14, 16, 19};
        timebase += cycles;
        s64 interval = (s64)4 << shift[io[0xa9] & 3];
        if (timebase >= interval) { timebase -= interval; io[0xa9] |= 0x08; }
        if (adc_busy > 0 && (adc_busy -= cycles) <= 0) { adc_busy = 0; io[0x2d] &= ~0x80; io[0x2d] |= 0x40; }
        update_irq();
    }

    long steps = 0;
    s64 cycles_total = 0;
    long irqs_taken = 0;
    T6963 lcd;

    void report() {
        printf("instructions %ld, cycles %lld (%.2f s), interrupts %ld, last PC %06X\n", steps, cycles_total, cycles_total / 16e6, irqs_taken, pc);
        std::vector<std::pair<long, u32>> top;
        for (auto& entry : hot) top.push_back({entry.second, entry.first});
        std::sort(top.rbegin(), top.rend());
        printf("hottest PCs:");
        for (size_t i = 0; i < top.size() && i < 10; i++) printf(" %06X x%ld", top[i].second, top[i].first);
        printf("\ndistinct PCs %zu\nlast PCs:", hot.size());
        for (int i = 0; i < 16; i++) printf(" %06X", trail[(trail_at + i) % 16]);
        puts("");
        lcd.report();
        for (auto& u : uart) {
            printf("%s: %ld bytes:", u.name, u.sent);
            for (u8 byte : u.head) printf(" %02X", byte);
            puts("");
        }
        printf("vectors taken:");
        for (auto& entry : vectors) printf(" %02X x%ld", entry.first, entry.second);
        puts("\nI/O registers (addr reads writes last firstPC):");
        for (auto& entry : iostat)
            printf("  %02X r%ld w%ld last %02X pc %06X\n", entry.first, entry.second.reads, entry.second.writes, entry.second.last, entry.second.first_pc);
        puts("external (16-byte block reads writes last firstPC):");
        for (auto& entry : ext)
            printf("  %06X r%ld w%ld last %02X pc %06X\n", entry.first, entry.second.reads, entry.second.writes, entry.second.last, entry.second.first_pc);
        puts("flash writes:");
        for (auto& entry : flashw)
            printf("  %06X w%ld last %02X pc %06X\n", entry.first, entry.second.writes, entry.second.last, entry.second.first_pc);
    }

protected:
    void debugger_instruction_hook(u32 address) override {
        pc = address;
        steps++;
        hot[address]++;
        trail[trail_at] = address;
        trail_at = (trail_at + 1) % 16;
    }
    int standard_irq_callback(int, u32) override {
        int vector = best_vector();
        if (vector) { irqs_taken++; vectors[vector]++; }
        return vector;
    }

private:
    bool source_pending(int vector) const {
        for (auto& t : timer) if (t.vector == vector) return t.pending();
        for (auto& u : uart) {
            if (u.tx_vector == vector) return u.tx_pending();
            if (u.rx_vector == vector) return u.rx_pending();
        }
        if (vector == 0x22) return (io[0xa9] & 0x18) == 0x18;  // time base
        if (vector == 0x1f) return (io[0x2d] & 0x60) == 0x60;  // A/D
        return false;
    }
    // The lowest level number wins; on a tie the lowest vector does.
    int best_vector() const {
        int best = 0, level = 7;
        for (int vector = 0x0b; vector <= 0x2a; vector++) {
            if (!source_pending(vector)) continue;
            int icr_level = io[0xb0 + (vector - 11) / 2] & 7;
            if (icr_level < level) { level = icr_level; best = vector; }
        }
        return best;
    }
    void update_irq() {
        int vector = best_vector();
        set_irq_level(vector ? io[0xb0 + (vector - 11) / 2] & 7 : 7);
    }

    u8 io_read(u32 address) {
        // Ports 1-A: output latches where DDR is set, pin levels elsewhere.
        // Inputs idle high: sheet 02/15 pulls the button lines up through AR1.
        if (address >= 0x01 && address <= 0x0a) {
            u8 ddr = io[address + 0x10];
            return (io[address] & ddr) | (0xff & ~ddr);
        }
        for (auto& t : timer) {
            if (address == (u32)t.base) return t.tmcsr & 0xfe;
            if (address == (u32)t.base + 1) return t.tmcsr >> 8;
            if (address == (u32)t.base + 2) return t.count & 0xff;
            if (address == (u32)t.base + 3) return t.count >> 8;
        }
        for (auto& u : uart) {
            if (address == (u32)u.base) return u.smr;
            if (address == (u32)u.base + 1) return u.scr;
            if (address == (u32)u.base + 2) return 0;
            if (address == (u32)u.base + 3) return u.ssr;
        }
        if (address >= 0xc0 && address <= 0xcf) return 0xff;  // keyboard, sheet 04/15, no keys down
        if (address == 0x2e) return 0x00;  // A/D result, mid scale
        if (address == 0x2f) return 0x02;
        return io[address];
    }
    void io_write(u32 address, u8 data) {
        for (auto& t : timer) {
            if (address == (u32)t.base) {
                bool trg = data & 1;
                t.tmcsr = (t.tmcsr & 0xff00) | (data & 0xfe);
                if (trg && (t.tmcsr & 2)) t.trigger();
                return;
            }
            if (address == (u32)t.base + 1) { t.tmcsr = (t.tmcsr & 0x00ff) | data << 8; return; }
            if (address == (u32)t.base + 2) { t.reload = (t.reload & 0xff00) | data; return; }
            if (address == (u32)t.base + 3) { t.reload = (t.reload & 0x00ff) | data << 8; return; }
        }
        for (auto& u : uart) {
            if (address == (u32)u.base) { u.smr = data; return; }
            if (address == (u32)u.base + 1) { u.scr = data; return; }
            if (address == (u32)u.base + 2) { u.write(data); return; }
            if (address == (u32)u.base + 3) { u.ssr = (u.ssr & 0xfc) | (data & 3); return; }
        }
        switch (address) {
        case 0xa1:  // CKSCR: MCM follows MCS once the clock has switched.
            io[address] = (data & 0xbf) | (data & 0x04 ? 0x40 : 0);
            break;
        case 0xa9:  // TBTC: writing 0 to TBOF clears it.
            io[address] = (data & ~0x08) | (data & io[address] & 0x08);
            break;
        case 0x2d:  // ADCS high: STRT bit starts a conversion.
            io[address] = data;
            if (data & 0x02) { adc_busy = 500; io[address] |= 0x80; io[address] &= ~0x02; }
            break;
        default:
            io[address] = data;
            break;
        }
    }

    void place(const std::string& path, u32 offset) {
        auto data = read_file(path);
        std::copy(data.begin(), data.end(), flash.begin() + offset);
    }
    Stat& touch(std::map<u32, Stat>& table, u32 key) {
        auto found = table.find(key);
        if (found != table.end()) return found->second;
        Stat& s = table[key];
        s.first_pc = pc;
        return s;
    }

    std::vector<u8> flash, ram;
    u8 internal[0x500];
    u8 io[0x100];
    ReloadTimer timer[2];
    Uart uart[3];
    s64 timebase = 0;
    int adc_busy = 0;
    u32 pc = 0;
    u32 trail[16] = {};
    int trail_at = 0;
    std::unordered_map<u32, long> hot;
    std::map<u32, Stat> iostat, ext, flashw;
    std::map<int, long> vectors;
};

int main(int argc, char** argv) {
    if (argc < 3) { puts("usage: ob12_master <rom dir> <instructions> [lcd.pbm]"); return 2; }
    auto* cpu = new Master();
    cpu->load(argv[1]);
    long limit = atol(argv[2]);
    cpu->attach(cpu);
    cpu->reset();
    try {
        while (cpu->steps < limit) {
            const int slice = 200;
            cpu->run(slice);
            cpu->tick(slice - cpu->cycles_left());
        }
    } catch (const std::exception& error) {
        printf("STOP: %s\n", error.what());
    }
    cpu->report();
    if (argc > 3) cpu->lcd.save_pbm(argv[3]);
    return 0;
}
