// TMS57070 DSP core for the OB-12 emulator.
//
// Written for this project with a flat integer design. Instruction behaviour was learned
// from the public Prehistoricman/TMS57070 reverse engineering (wiki, IDA module, emulator)
// and is checked against that emulator by tools/tms_harness/core_compare.cpp. See
// docs/HARDWARE.md for the licence position.
//
// Options switch between the public emulator's behaviour, used for that comparison, and
// behaviour inferred from the OB-12 firmware. Inferred behaviour is not checked on silicon.
#pragma once
#include <cstdint>
#include <map>
#include <vector>

namespace tms {

struct Options {
    bool audio_port_writes = false;  // secondary 0C/0D/0E with flag 4 write AX1/2/3
    bool true_dmem_ring = false;     // DMEM ring is 00..DCIRC, cells above stay fixed
    bool lock_delay = false;         // a DMEM lock from secondary 2E misses the next 27
    bool maclo_multiply = false;     // primary 43 multiplies by the low MAC word
    bool divide_1fc = false;         // primary 1FC divides by the last ALU operand
};

class Core {
public:
    explicit Core(const Options& options = Options());

    void reset();
    void step();
    void load_control(const uint8_t* header);  // 12 bytes, CR0-CR3, big-endian

    uint32_t pmem[512] = {};
    int32_t cmem[512] = {};
    int32_t dmem[512] = {};
    std::vector<int32_t> xmem;

    // Control registers, 24 bits each.
    uint32_t cr[4] = {};
    int32_t acc[2] = {};
    int64_t mac[2] = {}, mac_d1[2] = {}, mac_d2[2] = {};  // 52-bit, sign extended
    int32_t ar[2][2] = {};   // [port][0 left, 1 right]
    int32_t ax[3][2] = {};
    int32_t xrd = 0, t_reg = 0;
    uint32_t hir = 0;
    uint16_t pc = 0, rep_start = 0, rep_end = 0;
    uint32_t rptc = 0;
    uint16_t stack[4] = {};
    uint8_t sp = 0;
    uint16_t ca[2] = {}, da[2] = {}, cir[2] = {}, dir[2] = {};
    uint16_t coff = 0, ccirc = 0, doff = 0, dcirc = 0, goff = 0;
    uint32_t xoff = 0;
    bool bio = false;

    int32_t mac_upper(int64_t value) const;
    uint32_t mac_lower(int64_t value) const;

    // Encodings the core does not implement, for measurement.
    std::map<uint32_t, long> unknown;

private:
    bool flag(int reg, int bit) const { return (cr[reg] >> bit) & 1; }
    void set_flag(int reg, int bit, bool on) { cr[reg] = (cr[reg] & ~(1u << bit)) | ((uint32_t)on << bit); }
    int field(int reg, int shift, int bits) const { return (cr[reg] >> shift) & ((1 << bits) - 1); }

    uint32_t cmem_index(uint32_t address) const;
    uint32_t dmem_index(uint32_t address) const;
    uint32_t cmem_operand(uint32_t word) const;
    uint32_t dmem_operand(uint32_t word) const;
    uint32_t xmem_index(uint32_t address) const;

    void primary(uint32_t word);
    void secondary(uint32_t word);
    void post_increment(uint32_t word);
    void jump(uint32_t word);
    void pipeline();
    void queue_single(uint16_t* target, uint16_t value);
    void queue_dual(uint16_t* target, uint32_t value);

    int32_t settle_acc(int64_t value);
    void load_acc(uint32_t word, int operation);
    void alu(uint32_t word, int operation);
    void multiply(int unit, int64_t lhs, int64_t rhs, bool negate);
    void accumulate(int unit, int64_t lhs, int64_t rhs, bool negate);
    void mac_shift(int unit, int amount);
    void mac_set_upper(int unit, int32_t value);
    void mac_set_lower(int unit, uint32_t value);

    Options options;
    int output_shift() const;
    int round_bits() const;

    uint32_t alu_last = 0;
    bool lock_pending = false;
    int insn_cycles = 1;
    uint32_t xmem_read_address = 0;
    int xmem_read_cycles = 0;

    // Address register writes land two steps later.
    uint16_t* single_ptr[2] = {};
    uint16_t single_value[2] = {};
    uint16_t* dual_ptr[2] = {};
    uint32_t dual_value[2] = {};
};

}  // namespace tms
