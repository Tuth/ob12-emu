# OB-12 hardware and firmware facts

Each line says where it came from. Schematic pages are the service manual's own
`NN/15` numbers, read as rendered pages with vision, never by OCR.
`F:\MIZU\OB-12\OB-12-PROJECT-SUMMARY.md` is superseded where it disagrees with this file.

## Settled (schematic)

| Part | Detail | Source |
|---|---|---|
| Master CPU | IC12 MB90611APF, 4 MHz crystal, panel, keys, LCD, MIDI | 02/15 |
| Master flash | IC24 29F800T, 1 MB, CS0, F00000-FFFFFF | 03/15 |
| Master RAM | IC22/IC23 8128 SRAM, 256 KB, CS1, E00000-E1FFFF | 03/15 |
| Slave CPU | IC25 MB90611APF, 4 MHz crystal, debug RS232 test points | 06/15 |
| Slave flash | IC14 29F200T, 256 KB | 07/15 |
| Slave RAM | IC7/IC8 84256 SRAM, 64 KB | 07/15 |
| Clock | XTAL1 12.288 MHz = FSx256, so FS = 48 kHz | 08/15 |
| PLD | IC15 ATF1500-15J: D[7:0], A0, CSDSP, RD, WRL in; FS clocks, HCK[2:0], HR, HRS, HXS out; contents unknown | 08/15 |
| Voices | DASP0-3 = IC39, IC40, IC41, IC26 TMS57070, "DSP 24 bit", no external memory | 10/15 |
| LFO and effects | DASP4 = IC42 TMS57070 with 3x 44C256 DRAM on a 12-bit bus | 09/15 |
| Audio chain | DASPn AX2/AX3 feed DASPn+1 AR1/AR2; DASP3 feeds DASP4; DASP4 AX1 returns LFO to DASP0 AR1 | 09/15, 10/15 |
| DSP select | NDCS[4:0] on Slave P70-P74; shared HR/HX serial host bus, EMPTY and RDY back to Slave | 06/15, 10/15 |
| DSP clock | "PLL x 2" noted at DASP0: 24.576 MHz = 512 cycles per 48 kHz sample | 10/15 |

## Settled (measured from firmware)

- SysEx format, packing and checksum: see `tools/ob12_sysex.py`. All files pass.
- Master image: flash offset 0 = F00000. CALLP targets cluster in F0-F5, image ends F53D93.
- Boot block: flash offset FC000 = FFC000. Reset vector at FFFFDC: PC FFC012, mode 0x88.
- `unidasm -arch f2mc16` gives coherent code from FFC000 (stack, bus and chip-select setup).
- Slave image (`12dsp111`, section `[SoftSlave]`) is F2MC-16 code based at FC0000,
  not DSP code: string pointer FC00E0 hits "ERROR: interrupt IsrNull !!!".

## Settled (DSP microcode, carved by `tools/ob12_carve.py`)

The Slave holds parallel 10-entry tables at FC2340. All entries pass the layout
checks: header sits 12 bytes before PMEM, CMEM follows PMEM directly.

| Entry | PMEM | Words | CMEM | Words |
|---|---|---|---|---|
| 0 | FDB324 | 512 | FDBB24 | 487 |
| 1-3 (one shared block) | FDC0E6 | 512 | FDC8E6 | 487 |
| 4 | FDCEA8 | 512 | FDD6A8 | 313 |
| 5-8 (one shared block) | FDDD12 | 22 | FDDD6A | 3 |
| 9 | FCCFE8 | 10 | FCD010 | 1 |

- PMEM words are stored little-endian, CMEM words big-endian.
- Programs 0 and 1 differ in only 9 of 512 words.
- CMEM holds at least 487 words, so the wiki's 256-word CMEM cannot apply here.
- Static check (`tools/ob12_dsp_check.py`): primary opcodes 100% known in all programs,
  secondary 99.3-100%. Control on Slave CPU code: secondary 59.4%, jumps 0/23 in range.
- The three 512-word programs contain no 0xFx jump words at all.
- Secondary opcode 0x34 is absent from the public emulator's secondary switch, used by
  `6C341AB0` (programs 0, 1 at 11B, 186, 1F1) and `53F410D2` (program 4 at 01A, 048).
  The public IDA module does have an 0x34 branch.

## Open, not yet facts

- **Emulated run, 48000 samples per program, public emulator, no parameters from the Slave.**
  Not ground truth: the emulator is incomplete and the harness feeds a test sine.
  - Programs 0, 1, 4: all 512 words executed, 0 PC discontinuities, so straight-line
    execution with PC wrapping holds in this run. DMEM and CMEM change as it runs.
  - Speed: 26-35 ns per step, instrumented, one thread. Five DSPs need 123M steps per
    second, about 8 ns per step on one core, so a purpose-built core is needed.
  - Unimplemented paths hit, per sample: opcode 1FC divide step (30 in programs 0 and 1),
    0x3E with flag 8 (1 in programs 0 and 1), 0x43 MACLO multiply (3 in programs 0 and 1,
    2 in program 4), secondary 0x34 (3 in programs 0 and 1, 2 in program 4).
  - The emulator's 4-deep stack overflows, so the harness wraps it. The program has no
    return path used here; 192 interrupts fired in 48000 samples, then stopped. Not understood.
- **Audio output path, found by test.** Secondary 0x0C, 0x0D, 0x0E with flag 4 (bit 14)
  appear to write DMEM to AX1, AX2, AX3; flag 8 selects right. Without flag 4, 0x0C and
  0x0D read AR1, AR2. The public emulator treats these as reads only, and 0x0E as empty.
  Prediction tested on a patched local copy, 4000 frames, distinct sines on each input:
  - Program 1: AX3L = AR2L and AX3R = AR2R, 100% bit-exact at lag 0.
  - Program 0: AX3L = AR1L and AX3R = AR1R, 100%, matching 10/15 where DASP0's AR1 takes
    the LFO and forwards it down the AR2/AX3 bus.
  - Program 1: AX2L = AR1L 100% and AX2R = AR1R 93.7%, 2 half-frames late, with no voices
    playing. This is the summing bus.
  - Program 4 writes AX1, AX2, AX3, the pins 09/15 labels LFO, MOUT and AOUT.
  - Rates: AX2 and AX3 on program 4 write once per frame (48 kHz, as FSxd on the DAC
    side). AX L channels on the voice chain write twice per frame, matching the FSx2 clock.
  - Open: why AX2R is 93.7% rather than 100%; program 4 outputs were silent in this test.
- **Secondary 0x38.** One external write per sample in every large program, including
  voice DSPs with no external RAM. Purpose unknown.
- **Entry assignment hypothesis.** Entries 0-4 = DASP0-4 in normal use; 5-9 = short
  programs for the same chips, perhaps the Master's PROBEDASP tests. Needs the loader code.
- **12-byte header meaning.** Read as big-endian CR0-CR3, `3CF3CF B90100 FCFF00 030000`
  sets EXT=1 and EXTMEM=1, the 512-word CMEM mode that 487-word CMEM requires. DASP4's
  CR3 `250000` differs in the external bus width bits. Strong fit, but the loader is unread.
- **Slave boot block missing.** 167 of 1754 plausible CALLP targets land above the
  image, on 4 addresses: FFD3A0 (161 calls), FFD215, FFD12E, FFD1BD. The reset vector
  is also absent. `boot_152` at those offsets does not look like function entries.
  No supplied file contains this code. FFD3A0 falls mid-function in `boot_152`, so that
  file is not the Slave's. Likely factory-resident Slave code that updates never rewrite:
  the Master's own CPU update also stops at F53D93 and leaves FFC000 untouched, and the
  Slave image holds no flash-writing text or reset vector, so something resident must
  receive it. Inferred, not observed.
- **DMEM size.** The public emulator's EXT and EXTMEM logic gives 256-word DMEM with
  512-word CMEM, which would explain the wiki figure. Confirm from microcode addressing.
- **PLD behaviour** must be inferred from Slave code and the schematic.
- **Master to Slave link** runs over MSCK, MTX, STX; protocol unknown.

## Tooling and licences

- MAME `src/devices/cpu/f2mc16` includes an `MB90611A` device, BSD-3-Clause. Some
  opcodes only log "Unimplemented" (MULU, DIVU, CALLV and others).
- `Prehistoricman/TMS57070` has no licence file, so its code must not be copied.
  Get permission or write our own core. Local measurement runs only, allowed by Thor;
  nothing from it is in this repo. `tools/tms_harness/harness.cpp` is our own driver and
  needs a local copy of that emulator to build.
- `ioelectro/fujitsu-dev` holds Softune-style MB90 installers and an ISP example, no
  licence shown. Not installed. Possible later source of register headers or a simulator.
