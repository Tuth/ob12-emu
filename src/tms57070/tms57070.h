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
    bool noise_generator = false;    // secondary 34 stores a noise word; real sequence unknown
    bool late_mode_change = false;   // secondaries 28-2B act after the primary of the same word
    // study: 1 = the output shift travels with the product (shift at the end of the word that made it),
    // 2 = the shift in effect at the start of that word; 0 = applied when the MAC is read (old model)
    int shift_with_product = 0;
    int shift_paths = 3;  // with shift_with_product: 1 = stores and port writes, 2 = ALU and accumulator loads
    unsigned late_mode_mask = 0xf;   // which of 28, 29, 2A, 2B (bits 0-3) act late, with late_mode_change
    bool queued_pointer_load = false;  // secondaries 04-06, 08, 09 land two steps later, like C1-C5
    bool maclo_unsigned = false;     // primary 43 takes the low MAC word as unsigned
    bool maclo_shift = false;        // primary 43 product sits 24 bits lower, for double precision
    bool compare_3e = false;         // primary 3E flag 8 compares DMEM with CMEM, flags only
    uint32_t xmem_words = 0;  // the board's external RAM in words, a power of two; 0 = from CR3
    bool xread_flush = false;  // a new XMEM read lands the pending one in XRD first (read pipeline)
    // Words from an XMEM read to its data in XRD; 0 = the public emulator's table by CR3. The OB-12
    // effects program takes XRD 5 words after a read and expects the old value 2 words after one.
    int xread_cycles = 0;
    bool late_saturation_mode = false;  // secondary 2D acts after the primary, like 28-2B
    bool reuse_coefficient = false;  // primary 43 multiplies DMEM by the last multiplier C word
    bool bus_sum_saturates = false;  // the OB-12 voice output adds clip instead of wrapping
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
    int shift_d1[2] = {}, shift_d2[2] = {};  // output shift that goes with mac_d1, mac_d2 (shift_with_product)
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

    int32_t mac_upper(int64_t value) const { return mac_upper_s(value, output_shift()); }
    int32_t mac_upper_s(int64_t value, int shift) const;
    // study: per program address, how often a read sees a shift other than the one of its product's word
    mutable long shift_mismatch[512] = {};
    mutable uint32_t product_pc_d1[2] = {}, product_pc_d2[2] = {};
    std::vector<int> shift_pcs;  // study: product-time shift only for reads at these addresses (empty: all)
    bool product_shift_here(int path) const {
        if (!options.shift_with_product || !(options.shift_paths & path)) return false;
        if (shift_pcs.empty()) return true;
        int here = (pc - 1) & 0x1ff;
        for (int p : shift_pcs) if (p == here) return true;
        return false;
    }
    // The study switches cost nothing when off: the plain read is the model (see "The voice level
    // hangs on when the output shift takes effect" in docs/HARDWARE.md).
    int32_t upper_d2(int unit, int path = 1) const {
        if (!study_shift) return mac_upper(mac_d2[unit]);
        if (shift_d2[unit] != output_shift()) shift_mismatch[(pc - 1) & 0x1ff]++;
        return mac_upper_s(mac_d2[unit], product_shift_here(path) ? shift_d2[unit] : output_shift());
    }
    uint32_t lower_d2(int unit) const { return study_shift ? mac_lower_s(mac_d2[unit], product_shift_here(1) ? shift_d2[unit] : output_shift()) : mac_lower(mac_d2[unit]); }
    bool study_shift = false;  // the shift studies are on: set by the harness (OB12_SHIFT_*)
    uint32_t mac_lower(int64_t value) const { return mac_lower_s(value, output_shift()); }
    uint32_t mac_lower_s(int64_t value, int shift) const;

    // Encodings the core does not implement, for measurement.
    std::map<uint32_t, long> unknown;
    long wraps[512] = {};  // ACC overflows without saturation, by word

    // Study access: the CMEM and DMEM index a word would use now.
    uint32_t study_cmem(uint32_t word) const { return cmem_operand(word); }
    uint32_t study_dmem(uint32_t word) const { return dmem_operand(word); }
    uint32_t study_xmem(uint32_t address) const { return xmem_index(address); }
    uint32_t study_xread() const { return xmem_read_address; }

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
    void set_pointer(uint16_t* target, uint16_t value);

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

    int32_t last_coefficient = 0;
    uint32_t alu_last = 0;
    uint32_t noise_state = 0x1234567;
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
