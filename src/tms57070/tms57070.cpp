#include "tms57070.h"

namespace tms {

namespace {
enum { CR0, CR1, CR2, CR3 };
// CR1 bits
enum { AOV = 0, AOVL = 1, ACCZ = 2, ACCN = 3, MOV = 4, MOVL = 5, MOVR = 6, AOVM = 8, MOVM = 16, EXTMEM = 19, EXT = 20, LDMEM = 21, LCMEM = 23 };
// CR2 bits
enum { FREE = 17 };
// CR3 bits
enum { LXMEM = 15, XWORD = 16 };

enum { OpLoad, OpLoadUnsigned, OpOnes, OpTwos, OpInc, OpDec };
enum { OpAdd, OpSub, OpAnd, OpOr, OpXor, OpCmp };

inline int32_t s24(uint32_t value) { return (int32_t)(value << 8) >> 8; }
inline int64_t s52(int64_t value) { return (int64_t)((uint64_t)value << 12) >> 12; }
inline uint32_t u24(int32_t value) { return (uint32_t)value & 0xffffff; }
const uint64_t LOW24 = 0xffffff;
}  // namespace

Core::Core(const Options& chosen) : xmem(chosen.xmem_words > 0x10000 ? chosen.xmem_words : 0x10000, 0), options(chosen) { reset(); }

void Core::reset() {
    pc = 0;
    sp = 0;
    rptc = 0;
    xmem_read_address = 0;
    xmem_read_cycles = 0;
    cr[CR0] = 0xAA9BAD;
    cr[CR1] = 0x810100;
    cr[CR2] = 0x30FF00;
    cr[CR3] = 0xE50000;
    for (int i = 0; i < 2; i++) { single_ptr[i] = nullptr; dual_ptr[i] = nullptr; }
}

void Core::load_control(const uint8_t* h) {
    for (int i = 0; i < 4; i++) cr[i] = h[i * 3] << 16 | h[i * 3 + 1] << 8 | h[i * 3 + 2];
}

int Core::output_shift() const {
    static const int table[4] = {0, 2, 4, -8};
    return table[field(CR1, 12, 2)];
}

int Core::round_bits() const {
    static const int table[8] = {0, 24, 28, 30, 32, 32, 28, 30};
    return table[field(CR1, 9, 3)];
}

int32_t Core::mac_upper_s(int64_t value, int shift) const {
    int64_t v = shift >= 0 ? value << shift : value >> -shift;
    int bits = round_bits();
    int64_t mask = 0;
    if (bits) {
        int64_t carry = v & ((int64_t)1 << (bits - 1));
        mask = ((int64_t)1 << bits) - 1;
        v += carry << 1;
    }
    int64_t upper = v >> 24;
    if (flag(CR1, MOVM)) {
        if (upper > 0x7fffff) upper = 0x7fffff;
        else if (upper < -0x800000) upper = -0x800000;
    }
    if (bits) upper &= ~(mask >> 24);
    return s24((uint32_t)upper);
}

uint32_t Core::mac_lower_s(int64_t value, int shift) const {
    int64_t v = shift >= 0 ? value << shift : value >> -shift;
    uint32_t lower = (uint32_t)(v & LOW24);
    if (flag(CR1, MOVM)) {
        int64_t upper = v >> 24;
        if (upper > 0x7fffff) lower = 0xffffff;
        else if (upper < -0x800000) lower = 0;
    }
    return lower;
}

uint32_t Core::cmem_index(uint32_t address) const {
    uint32_t a = address & 0xffff;
    if (!flag(CR1, LCMEM)) a = (a + coff) & 0xffff;
    return a & ((flag(CR1, EXT) && flag(CR1, EXTMEM)) ? 0x1ff : 0xff);
}

uint32_t Core::dmem_index(uint32_t address) const {
    uint32_t a = address & 0xffff;
    uint32_t size_mask = (flag(CR1, EXT) && !flag(CR1, EXTMEM)) ? 0x1ff : 0xff;
    if (!flag(CR1, LDMEM)) {
        if (options.true_dmem_ring) {
            a &= size_mask;
            if (a <= dcirc) { a += doff; if (a > dcirc) { a -= dcirc + 1u; if (a > dcirc) a %= dcirc + 1u; } }
            return a;
        }
        a = (a + doff) & 0xffff;
    }
    return a & size_mask;
}

uint32_t Core::cmem_operand(uint32_t word) const {
    uint32_t address = 0;
    switch ((word >> 12) & 3) {
    case 1: address = ca[(word & 0x800) ? 1 : 0]; break;
    case 2: address = word; break;
    case 3: address = ca[(word & 0x100) ? 1 : 0]; break;
    }
    return cmem_index(address);
}

uint32_t Core::dmem_operand(uint32_t word) const {
    uint32_t address = 0;
    switch ((word >> 12) & 3) {
    case 1: address = word; break;
    case 2: case 3: address = da[(word & 0x800) ? 1 : 0]; break;
    }
    return dmem_index(address);
}

uint32_t Core::xmem_index(uint32_t address) const {
    static const uint32_t sizes[4] = {0x4000, 0x8000, 0x10000, 0x10000};
    uint32_t size = sizes[field(CR3, 17, 2)];
    if (flag(CR3, XWORD)) size >>= 1;
    if (options.xmem_words) size = options.xmem_words;
    return (address + xoff) & (size - 1);
}

int32_t Core::settle_acc(int64_t value) {
    if (flag(CR1, AOVM)) {
        if (value < -0x800000) { value = -0x800000; set_flag(CR1, AOV, true); }
        else if (value > 0x7fffff) { value = 0x7fffff; set_flag(CR1, AOV, true); }
    } else if (value < -0x800000 || value > 0x7fffff) {
        set_flag(CR1, AOV, true);
        wraps[(pc - 1) & 0x1ff]++;
    }
    int32_t result = s24((uint32_t)value);
    set_flag(CR1, ACCZ, result == 0);
    set_flag(CR1, ACCN, result < 0);
    return result;
}

void Core::load_acc(uint32_t word, int operation) {
    bool flag4 = word & 0x400000, flag8 = word & 0x800000;
    int32_t value = 0;
    switch ((word >> 24) & 3) {
    case 0: value = dmem[dmem_operand(word)]; break;
    case 1: value = cmem[cmem_operand(word)]; break;
    case 2: value = acc[flag8]; break;
    case 3: value = upper_d2(flag8); break;
    }
    switch (operation) {
    case OpLoadUnsigned: if (value < 0) value = -value; break;
    case OpTwos: value = -value; break;
    case OpOnes: value = ~value; break;
    case OpInc: value++; break;
    case OpDec: value--; break;
    }
    acc[flag4] = settle_acc(value);
}

void Core::alu(uint32_t word, int operation) {
    bool flag4 = word & 0x400000, flag8 = word & 0x800000;
    uint32_t top = word >> 24;
    int32_t lhs, rhs;
    if (top >= 0x3c && top <= 0x3e) {
        lhs = dmem[dmem_operand(word)];
        rhs = cmem[cmem_operand(word)];
    } else {
        int source = top & 3;
        lhs = source < 2 ? dmem[dmem_operand(word)] : cmem[cmem_operand(word)];
        rhs = (source & 1) ? upper_d2(flag8) : acc[flag8];
    }
    if (operation == OpAdd || operation == OpSub || operation == OpCmp) alu_last = u24(lhs);
    int64_t result = 0;
    switch (operation) {
    case OpAdd: result = (int64_t)lhs + rhs; break;
    case OpSub: case OpCmp: result = (int64_t)lhs - rhs; break;
    case OpAnd: result = lhs & rhs; break;
    case OpOr: result = lhs | rhs; break;
    case OpXor: result = lhs ^ rhs; break;
    }
    if (operation == OpCmp) {
        set_flag(CR1, AOV, result < -0x800000 || result > 0x7fffff);
        set_flag(CR1, ACCZ, result == 0);
        set_flag(CR1, ACCN, result < 0);
        return;
    }
    acc[flag4] = settle_acc(result);
}

// Product of two 24-bit fractions, aligned as a MAC value: a * b * 2.
void Core::multiply(int unit, int64_t lhs, int64_t rhs, bool negate) {
    int64_t product = lhs * rhs * 2;
    mac[unit] = s52(negate ? -product : product);
}

void Core::accumulate(int unit, int64_t lhs, int64_t rhs, bool negate) {
    int64_t current = mac[unit];
    switch (field(CR1, 14, 2)) {
    case 1: current <<= 2; break;
    case 2: current <<= 4; break;
    case 3: current >>= 24; break;
    }
    multiply(unit, lhs, rhs, negate);
    mac[unit] = s52(mac[unit] + current);
}

void Core::mac_shift(int unit, int amount) {
    mac[unit] = s52(amount < 0 ? mac[unit] >> -amount : mac[unit] << amount);
}

void Core::mac_set_upper(int unit, int32_t value) {
    uint64_t raw = (uint64_t)mac[unit] & LOW24;
    raw |= (uint64_t)((int64_t)value << 24);
    mac[unit] = s52((int64_t)raw);
}

void Core::mac_set_lower(int unit, uint32_t value) {
    uint64_t raw = (uint64_t)mac[unit] & ~LOW24;
    raw |= value & LOW24;
    mac[unit] = s52((int64_t)raw);
}

void Core::queue_single(uint16_t* target, uint16_t value) { single_ptr[0] = target; single_value[0] = value & 0xfff; }
void Core::queue_dual(uint16_t* target, uint32_t value) { dual_ptr[0] = target; dual_value[0] = value & 0xffffff; }
void Core::set_pointer(uint16_t* target, uint16_t value) {
    if (options.queued_pointer_load) queue_single(target, value);
    else *target = value;
}

void Core::pipeline() {
    if (dual_ptr[1]) { dual_ptr[1][0] = dual_value[1] & 0xfff; dual_ptr[1][1] = (dual_value[1] >> 12) & 0xfff; }
    if (single_ptr[1]) *single_ptr[1] = single_value[1];
    dual_ptr[1] = dual_ptr[0]; dual_value[1] = dual_value[0];
    single_ptr[1] = single_ptr[0]; single_value[1] = single_value[0];
    dual_ptr[0] = nullptr;
    single_ptr[0] = nullptr;
}

static inline int64_t as_unsigned(int32_t value) { return (int64_t)(uint32_t)u24(value); }

void Core::primary(uint32_t word) {
    uint32_t op = word >> 24;
    bool flag4 = word & 0x400000, flag8 = word & 0x800000;
    int unit = flag4;  // MAC unit for multiply groups
    bool negate = flag8;

    auto pick = [](int32_t value, bool is_unsigned) { return is_unsigned ? as_unsigned(value) : (int64_t)value; };

    if (op < 0x04) return;
    if (op < 0x08) { load_acc(word, OpLoadUnsigned); return; }
    if (op < 0x0c) { load_acc(word, OpTwos); return; }
    if (op < 0x10) { load_acc(word, OpOnes); return; }
    if (op < 0x14) { load_acc(word, OpLoad); return; }
    if (op < 0x18) { load_acc(word, OpInc); return; }
    if (op < 0x1c) { load_acc(word, OpDec); return; }
    if (op >= 0x20 && op < 0x24) { alu(word, OpAdd); return; }
    if (op >= 0x24 && op < 0x28) { alu(word, OpSub); return; }
    if (op >= 0x28 && op < 0x2c) { alu(word, OpAnd); return; }
    if (op >= 0x2c && op < 0x30) { alu(word, OpOr); return; }
    if (op >= 0x30 && op < 0x34) { alu(word, OpXor); return; }
    if (op >= 0x34 && op < 0x38) { alu(word, OpCmp); return; }

    switch (op) {
    case 0x1c: {
        int64_t value = acc[flag4];
        value = flag8 ? value << 1 : value >> 1;
        acc[flag4] = settle_acc(value);
        break;
    }
    case 0x1d:
        if (flag8) { unknown[word & 0xffc00000]++; break; }
        acc[flag4] = 0;
        settle_acc(0);
        break;
    case 0x1e:
        if (flag4) { unknown[word & 0xffc00000]++; break; }
        acc[1] = mac_upper(mac[flag8]);
        acc[0] = s24(mac_lower(mac[flag8]));
        break;
    case 0x1f:
        if (flag8) {
            uint32_t divisor;
            if (flag4) {
                if (!options.divide_1fc) { unknown[word & 0xffc00000]++; break; }
                divisor = alu_last;
            } else {
                divisor = u24(dmem[dmem_operand(word)]);
                alu_last = divisor;
            }
            uint32_t low = u24(acc[0]), high = u24(acc[1]);
            uint32_t difference = (high - divisor) & 0xffffff;
            bool subtract = !(difference & 0x800000);
            if (options.divide_1fc && divisor == 0) subtract = false;  // hypothesis, see docs
            if (subtract) high = difference;
            acc[1] = s24((high << 1) | (low >> 23));
            acc[0] = s24((low << 1) | (subtract ? 1 : 0));
        } else if (!flag4) {
            acc[0] = acc[1] = 0;
            settle_acc(0);
        }
        break;
    case 0x38: {
        int unit2 = flag8;
        int32_t upper = mac_upper(mac[unit2]);
        if (!(upper >= 0x400000 || upper < -0x400000)) {
            mac_shift(unit2, 1);
            acc[flag4] = s24((uint32_t)(acc[flag4] - 1));
        }
        break;
    }
    case 0x39:
        if (flag4) {
            xmem[xmem_index((uint32_t)cmem[cmem_operand(word)])] = dmem[dmem_operand(word)];
        } else {
            static const int with_word[4] = {21, 12, 9, 0};
            static const int without[4] = {15, 9, 0, 6};
            if (options.xread_flush && xmem_read_cycles != 0) {
                xrd = xmem[xmem_read_address];
                if (!flag(CR3, XWORD)) xrd = s24(u24(xrd) & 0xffff00);
            }
            xmem_read_address = xmem_index((uint32_t)cmem[cmem_operand(word)]);
            int bus = field(CR3, 17, 2);
            xmem_read_cycles = flag(CR3, XWORD) ? with_word[bus] : without[bus];
        }
        break;
    case 0x3a: case 0x3b: case 0x3f: break;
    case 0x3c: alu(word, flag8 ? OpSub : OpAdd); break;
    case 0x3d: alu(word, flag8 ? OpOr : OpAnd); break;
    case 0x3e:
        if (flag8 && !options.compare_3e) unknown[word & 0xffc00000]++;
        else alu(word, flag8 ? OpCmp : OpXor);
        break;

    case 0x40: case 0x41: case 0x44: case 0x45: case 0x48: case 0x49: case 0x4c: case 0x4d: {
        int32_t a = acc[op & 1];
        int32_t c = cmem[cmem_operand(word)];
        last_coefficient = c;
        multiply(unit, pick(c, op & 4), pick(a, op & 8), negate);
        break;
    }
    case 0x42: case 0x46: case 0x4a: case 0x4e: {
        int32_t c = cmem[cmem_operand(word)];
        int32_t d = dmem[dmem_operand(word)];
        last_coefficient = c;
        multiply(unit, pick(c, op & 4), pick(d, op & 8), negate);
        break;
    }
    case 0x43: case 0x47: case 0x4b: case 0x4f:
        if (options.reuse_coefficient) {
            // Always paired with a secondary that takes the C bus; the multiplier keeps its last C.
            multiply(unit, pick(last_coefficient, op & 4), pick(dmem[dmem_operand(word)], op & 8), negate);
        } else if (options.maclo_multiply) {
            int32_t d = dmem[dmem_operand(word)];
            bool is_unsigned = op & 8;
            int64_t low = options.maclo_unsigned ? (int64_t)mac_lower(mac[unit]) : (int64_t)s24(mac_lower(mac[unit]));
            multiply(unit, low, pick(d, is_unsigned), negate);
            if (options.maclo_shift) mac[unit] >>= 24;
        } else {
            unknown[word & 0xff000000]++;
        }
        break;

    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
        int32_t a = acc[op & 1];
        bool from_dmem = op & 2;
        int32_t w = from_dmem ? dmem[dmem_operand(word)] : cmem[cmem_operand(word)];
        bool acc_unsigned = false, word_unsigned = false;
        switch (op & 0xc) {
        case 0x4: if (from_dmem) acc_unsigned = true; else word_unsigned = true; break;
        case 0x8: if (from_dmem) word_unsigned = true; else acc_unsigned = true; break;
        case 0xc: acc_unsigned = word_unsigned = true; break;
        }
        accumulate(unit, pick(a, acc_unsigned), pick(w, word_unsigned), negate);
        break;
    }
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: {
        int32_t a = acc[op & 1];
        bool from_dmem = op & 2;
        int32_t w = from_dmem ? dmem[dmem_operand(word)] : cmem[cmem_operand(word)];
        bool acc_unsigned = (op & 4) && from_dmem, word_unsigned = (op & 4) && !from_dmem;
        if (field(CR1, 14, 2) == 0) mac_shift(unit, -24);
        accumulate(unit, pick(a, acc_unsigned), pick(w, word_unsigned), negate);
        break;
    }
    case 0x68: case 0x69: case 0x6a: case 0x6b: break;
    case 0x6c: case 0x6d: case 0x6e: case 0x6f: {
        int32_t c = cmem[cmem_operand(word)];
        int32_t d = dmem[dmem_operand(word)];
        last_coefficient = c;
        accumulate(unit, pick(c, op & 1), pick(d, op & 2), negate);
        break;
    }
    case 0x70: case 0x71: {
        int32_t c = cmem[cmem_operand(word)];
        int32_t d = dmem[dmem_operand(word)];
        last_coefficient = c;
        if (field(CR1, 14, 2) == 0) mac_shift(unit, -24);
        accumulate(unit, pick(c, op & 1), d, negate);
        break;
    }
    case 0x72: mac_shift(unit, flag8 ? 1 : -1); break;
    case 0x73:
        if (flag8) unknown[word & 0xffc00000]++;
        else mac[unit] = 0;
        break;
    case 0x74:
        if (flag8 || flag4) { unknown[word & 0xffc00000]++; break; }
        mac[0] = mac[1] = 0;
        mac_d1[0] = mac[0];
        mac_d1[1] = mac[1];
        shift_d1[0] = shift_d1[1] = output_shift();
        break;
    case 0x75: case 0x76: case 0x77: case 0x7e: case 0x7f: break;
    case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: {
        if (op < 0x7a) mac_set_lower(unit, 0);
        int32_t value;
        if ((op & 1) == 0) value = flag8 ? acc[0] : dmem[dmem_operand(word)];
        else value = flag8 ? acc[1] : cmem[cmem_operand(word)];
        if (op < 0x7c) mac_set_upper(unit, value);
        else mac_set_lower(unit, (uint32_t)value);
        mac_d1[unit] = mac[unit];
        shift_d1[unit] = output_shift();
        break;
    }

    case 0xc1: {
        uint16_t value = word & 0xfff;
        switch ((word >> 16) & 0xff) {
        case 0x00: queue_single(&da[0], value); break;
        case 0x08: queue_single(&da[1], value); break;
        case 0x10: queue_single(&dir[0], value); break;
        case 0x18: queue_single(&dir[1], value); break;
        case 0x20: queue_single(&ca[0], value); break;
        case 0x28: queue_single(&ca[1], value); break;
        case 0x30: queue_single(&cir[0], value); break;
        case 0x38: queue_single(&cir[1], value); break;
        case 0x40: if (!flag(CR1, ACCN)) queue_single(&ca[0], value); break;
        case 0x48: if (!flag(CR1, ACCN)) queue_single(&ca[1], value); break;
        default: unknown[word & 0xffff0000]++; break;
        }
        break;
    }
    case 0xc2: queue_dual(da, word); break;
    case 0xc3: queue_dual(dir, word); break;
    case 0xc4: queue_dual(ca, word); break;
    case 0xc5: queue_dual(cir, word); break;
    case 0xc6: if (!flag(CR1, ACCN)) queue_dual(ca, word); break;
    case 0xc7: doff = word & 0xfff; dcirc = (word >> 12) & 0xfff; break;
    case 0xc8: coff = word & 0xfff; ccirc = (word >> 12) & 0xfff; break;
    case 0xca: acc[0] = s24(word); settle_acc(acc[0]); break;
    case 0xcb: acc[1] = s24(word); settle_acc(acc[1]); break;
    case 0xcc: cr[CR0] = word & 0xffffff; break;
    case 0xcd: cr[CR1] = word & 0xffffff; break;
    case 0xce: {
        uint32_t value = word & 0xffffff;
        uint32_t common = value & cr[CR2] & 0xff;
        cr[CR2] = (value & 0xffff00) | ((cr[CR2] & 0xff) ^ common);
        break;
    }
    case 0xcf: cr[CR3] = word & 0xffffff; break;
    case 0xe0: rptc = (word >> 16) & 0xff; rep_start = rep_end = pc; insn_cycles = rptc ? 1 : 2; break;
    case 0xe2: rptc = (uint8_t)acc[0]; rep_start = rep_end = pc; insn_cycles = rptc ? 3 : 4; break;
    case 0xe3: rptc = (uint8_t)acc[1]; rep_start = rep_end = pc; insn_cycles = rptc ? 3 : 4; break;
    case 0xe4:
        rptc = (word >> 16) & 0xff;
        rep_start = pc;
        rep_end = word & 0x1ff;
        if (rep_end == pc) rptc = 0;
        break;
    case 0xec: sp--; pc = stack[sp & 3]; insn_cycles = 2; break;
    case 0xee: sp--; pc = stack[sp & 3]; set_flag(CR2, FREE, true); rptc = 0; insn_cycles = 2; break;
    default:
        if (op >= 0xf0) jump(word);
        else unknown[word & 0xff000000]++;
        break;
    }
}

void Core::jump(uint32_t word) {
    uint32_t condition = (word >> 20) & 0x7c;
    bool is_call = (word >> 24) >= 0xf8;
    bool taken = false;
    uint16_t target = (uint16_t)word;
    switch (condition) {
    case 0x00: taken = true; break;
    case 0x08: taken = true; target = (uint16_t)acc[0]; break;
    case 0x0c: taken = true; target = (uint16_t)acc[1]; break;
    case 0x10: taken = flag(CR1, ACCZ); break;
    case 0x18: taken = !flag(CR1, ACCZ); break;
    case 0x20: taken = !(flag(CR1, ACCZ) || flag(CR1, ACCN)); break;
    case 0x28: taken = flag(CR1, ACCN); break;
    case 0x30: taken = flag(CR1, AOV); break;
    case 0x38: taken = flag(CR1, AOVL); break;
    case 0x40: taken = flag(CR1, MOV); break;
    case 0x48: taken = flag(CR1, MOVL); break;
    case 0x50: taken = flag(CR1, MOVR); break;
    case 0x58: taken = bio; break;
    default: unknown[word & 0xfff00000]++; break;
    }
    insn_cycles = condition == 0 ? 2 : (condition == 8 || condition == 0xc) ? 4 : 3;
    if (taken) {
        if (is_call) { stack[sp & 3] = pc; sp++; }
        pc = target & 0x1ff;
    }
}

void Core::secondary(uint32_t word) {
    uint32_t op = (word >> 16) & 0x3f;
    bool flag4 = word & 0x4000, flag8 = word & 0x8000;
    int args = (word >> 14) & 3;
    int32_t* target = nullptr;

    switch (op) {
    case 0x00: break;
    case 0x01:
        if (flag8) cmem[cmem_operand(word)] = acc[flag4];
        else dmem[dmem_operand(word)] = acc[flag4];
        break;
    case 0x02:
        if (flag8) cmem[cmem_operand(word)] = upper_d2(flag4);
        else dmem[dmem_operand(word)] = upper_d2(flag4);
        break;
    case 0x03:
        if (flag8) cmem[cmem_operand(word)] = s24(lower_d2(flag4));
        else dmem[dmem_operand(word)] = s24(lower_d2(flag4));
        break;
    case 0x04: set_pointer(&da[flag8], acc[flag4] & 0xfff); break;
    case 0x05: set_pointer(&ca[flag8], acc[flag4] & 0xfff); break;
    case 0x06: {
        uint32_t value = (uint32_t)cmem[cmem_operand(word)];
        uint16_t* pair = args == 0 ? da : args == 1 ? dir : args == 2 ? ca : cir;
        if (options.queued_pointer_load) queue_dual(pair, value);
        else { pair[0] = value & 0xfff; pair[1] = (value >> 12) & 0xfff; }
        break;
    }
    case 0x07: {
        uint16_t* pair = args == 0 ? da : args == 1 ? dir : args == 2 ? ca : cir;
        cmem[cmem_operand(word)] = s24((uint32_t)pair[1] << 12 | pair[0]);
        break;
    }
    case 0x08: set_pointer(&(flag4 ? dir : da)[flag8], cmem[cmem_operand(word)] & 0xfff); break;
    case 0x09: set_pointer(&(flag4 ? cir : ca)[flag8], cmem[cmem_operand(word)] & 0xfff); break;
    case 0x0a: case 0x0b: {
        uint16_t* regs[4][2] = {{&da[0], &ca[0]}, {&dir[0], &cir[0]}, {&da[1], &ca[1]}, {&dir[1], &cir[1]}};
        cmem[cmem_operand(word)] = *regs[args][op == 0x0b];
        break;
    }
    case 0x0c: case 0x0d: {
        int port = op == 0x0d;
        if (flag4 && options.audio_port_writes) {
            ax[port][flag8] = dmem[dmem_operand(word)];
        } else {
            dmem[dmem_operand(word)] = ar[port][flag8];
        }
        break;
    }
    case 0x0e: case 0x0f:
        if (op == 0x0e && flag4 && options.audio_port_writes) ax[2][flag8] = dmem[dmem_operand(word)];
        else dmem[dmem_operand(word)] = 0;
        break;
    case 0x18: case 0x19: case 0x1a:
        ax[op - 0x18][flag8] = upper_d2(flag4);
        break;
    case 0x20:
        switch (args) {
        case 0: t_reg = dmem[dmem_operand(word)]; break;
        case 2: dmem[dmem_operand(word)] = xrd; break;
        default: unknown[word & 0x3fc000]++; break;
        }
        break;
    case 0x21:
        if (args == 0) { doff = (doff - 1) & 0xfff; goff = (goff + 1) & 0xfff; }
        else if (args == 2) goff = 0;
        break;
    case 0x22:
        cr[args] = u24(cmem[cmem_operand(word)]);
        break;
    case 0x23: cmem[cmem_operand(word)] = s24(cr[args]); break;
    case 0x26: hir = u24(flag8 ? cmem[cmem_operand(word)] : dmem[dmem_operand(word)]); break;
    case 0x27:
        if (!flag(CR1, LCMEM)) {
            int32_t end = cmem[cmem_index(ccirc)];
            coff = (coff - 1) & 0xfff;
            cmem[cmem_index(0)] = end;
        }
        if (!flag(CR1, LDMEM) || lock_pending) {
            if (options.true_dmem_ring) {
                doff = (doff + dcirc) % (dcirc + 1u);
            } else {
                int32_t end = dmem[dmem_index(dcirc)];
                doff = (doff - 1) & 0xfff;
                dmem[dmem_index(0)] = end;
            }
            lock_pending = false;
        }
        if (!flag(CR3, LXMEM)) xoff--;
        goff = (goff + 1) & 0xfff;
        ca[1] = 0;
        da[1] = 0;
        break;
    case 0x28: cr[CR1] = (cr[CR1] & ~(3u << 14)) | ((uint32_t)args << 14); break;
    case 0x29: cr[CR1] = (cr[CR1] & ~(3u << 12)) | ((uint32_t)args << 12); break;
    case 0x2a: case 0x2b: {
        uint32_t mode = args + (op == 0x2b ? 4 : 0);
        cr[CR1] = (cr[CR1] & ~(7u << 9)) | (mode << 9);
        break;
    }
    case 0x2c:
        if (flag8) {
            if (flag4) { set_flag(CR1, MOV, false); set_flag(CR1, MOVL, false); }
            else { set_flag(CR1, AOV, false); set_flag(CR1, AOVL, false); }
        } else {
            set_flag(CR2, FREE, flag4);
        }
        break;
    case 0x2d: set_flag(CR1, flag8 ? MOVM : AOVM, flag4); break;
    case 0x2e:
        if (flag8) {
            set_flag(CR1, LCMEM, flag4);
        } else {
            lock_pending = options.lock_delay && flag4 && !flag(CR1, LDMEM);
            set_flag(CR1, LDMEM, flag4);
        }
        break;
    case 0x30: case 0x31: case 0x32: case 0x33:
        dmem[dmem_operand(word)] = xrd;
        break;
    case 0x34:
        if (options.noise_generator) {
            noise_state ^= noise_state << 13;
            noise_state ^= noise_state >> 17;
            noise_state ^= noise_state << 5;
            dmem[dmem_operand(word)] = s24(noise_state);
        } else {
            unknown[0x80000000u | (op << 16)]++;
        }
        break;
    case 0x38: case 0x39: case 0x3a: case 0x3b:
        break;
    default:
        unknown[0x80000000u | (op << 16)]++;
        break;
    }
    (void)target;
}

void Core::post_increment(uint32_t word) {
    int mode = (word >> 12) & 3;
    int high = (word >> 8) & 0xf, low = (word >> 4) & 0xf;
    if (mode & 2) {
        uint16_t& reg = da[(high & 8) ? 1 : 0];
        if (high & 4) reg = (reg + dir[(high & 2) ? 1 : 0]) & 0xfff;
        else if (high & 2) reg = (reg + 1) & 0xfff;
    }
    if (mode == 1) {
        uint16_t& reg = ca[(high & 8) ? 1 : 0];
        if (high & 4) reg = (reg + cir[(high & 2) ? 1 : 0]) & 0xfff;
        else if (high & 2) reg = (reg + 1) & 0xfff;
    } else if (mode == 3) {
        uint16_t& reg = ca[(high & 1) ? 1 : 0];
        if (low & 8) reg = (reg + cir[(low & 4) ? 1 : 0]) & 0xfff;
        else if (low & 4) reg = (reg + 1) & 0xfff;
    }
}

void Core::step() {
    uint32_t word = pmem[pc];
    // The voice output adds of the OB-12 programs, see "Bus sum saturates" in docs/HARDWARE.md.
    bool bus_sum = options.bus_sum_saturates && (word == 0xa13b2122 || word == 0xa13b2139 || word == 0x2186a136 || word == 0x2186a14d || word == 0x210921e2 || word == 0x21a6b000);
    bool plain = flag(CR1, AOVM);
    if (bus_sum) set_flag(CR1, AOVM, true);
    if (rptc && pc == rep_end) { pc = rep_start; rptc--; }
    else pc = (pc + 1) & 0x1ff;

    pipeline();
    insn_cycles = 1;
    int shift_start = output_shift();

    uint32_t top = word >> 24;
    if (top >= 0xc0) {
        primary(word);
    } else if (top >= 0x80) {
        uint32_t paired = (word & 0x3fff) | (((word & 0x3fc000) + 0x400000) << 8);
        primary(paired);
        word &= 0x7fffffff;
        primary(word);
        post_increment(word);
    } else {
        uint32_t op = (word >> 16) & 0x3f;
        if (options.late_mode_change && ((op >= 0x28 && op <= 0x2b && (options.late_mode_mask >> (op - 0x28) & 1)) || (options.late_saturation_mode && op == 0x2d))) {
            primary(word);
            secondary(word);
        } else {
            secondary(word);
            primary(word);
        }
        post_increment(word);
    }

    if (bus_sum) set_flag(CR1, AOVM, plain);

    for (int unit = 0; unit < 2; unit++) {
        mac_d2[unit] = mac_d1[unit];
        mac_d1[unit] = mac[unit];
        shift_d2[unit] = shift_d1[unit];
        shift_d1[unit] = options.shift_with_product == 2 ? shift_start : output_shift();
    }

    if (xmem_read_cycles != 0) {
        xmem_read_cycles -= insn_cycles;
        if (xmem_read_cycles <= 0) {
            xmem_read_cycles = 0;
            xrd = xmem[xmem_read_address];
            if (!flag(CR3, XWORD)) xrd = s24(u24(xrd) & 0xffff00);
        }
    }
}

}  // namespace tms
