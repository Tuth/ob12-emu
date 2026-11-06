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
  - Program 1: AX2L = AR1L and AX2R = AR1R, 2 half-frames late, with no voices playing.
    This is the summing bus. The first 93.7% for AX2R was a test artefact: compared on even
    half frames, 0 of 7968 differ.
  - Program 4 writes AX1, AX2, AX3, the pins 09/15 labels LFO, MOUT and AOUT.
  - Rates: AX2 and AX3 on program 4 write once per frame (48 kHz, as FSxd on the DAC
    side). AX L channels on the voice chain write twice per frame, matching the FSx2 clock.
- **Five-DSP chain, `tools/tms_harness/chain_test.cpp`, 8000 frames, wired per 09/15, 10/15.**
  Links are copied once per half frame, a model choice, so lags are model values.
  - Summing bus: tone into DASP1 AR1 reaches DASP4 AR1L 100% bit-exact, lag 9 half frames,
    stepping 3 per DSP. DASP4 AR1R best match 87.5% in the report; only DASP1's right
    channel was checked on matched parity.
  - LFO bus: tone into DASP0 AR1 reaches DASP4 AR2L and AR2R 100% bit-exact, lag 4 and 8.
  - Program 4 never reads AR2, so the LFO bus ends at DASP4.
  - 26.5 ns per DSP step across the chain.
- **DASP4 outputs stay silent.** Input reaches DMEM (D0D0, D0D1, copies at D053 onward).
  Outputs come from D017, D018 (AX2) and D00B, D00F (AX3), through CMEM pointers 7D-80,
  fed by a MAC chain at 060-06F whose accumulators stay zero. Setting each of the 180 zero
  CMEM words to 0x400000, singly and all at once, opened nothing. That test is weak: it
  also overwrites pointers and cannot change nonzero words. Cause unknown; candidates are
  Slave-written state, the unimplemented opcodes, or DMEM state set by the Slave.
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

## Master boot, `src/ob12_master.cpp` (measured)

300M instructions, 76.8 s emulated, no crash. Real firmware, no patches to it.

- Reset at FFC012, boot hands over to the F00000 image, LCD splash drawn (35127 writes).
- RTOS with 9 tasks; stacks at EC:87F4 upward, switch by `MOVW SP,A` then `RETI` at F01C76.
- Idle loop at F00709 takes most cycles.
- UART1 sends MIDI Active Sensing `FE`: 257 bytes in 76.8 s, about one per 300 ms.
- A/D: 8-channel scan by EI2OS, 140032 transfers. Time base and reload timers all firing.
- UART0 (LED) and UART2 (Slave) have sent 0 bytes. The display stays on the splash with an
  empty progress bar. What it waits for is not yet known; the Slave is the prime suspect.
- The stall on the splash is solved. Causes, in order found:
  - Factory timbres and programs were not loaded. They belong at flash offsets C0000 and
    D0000, from `12tmbFS.mid` and `12prgFS.mid` in the presets zip.
  - The firmware then formats user flash, so the 29F800 needed a command model.
  - Upstream bug: `CALLP @RWx+disp8` pushed return address +4 for a 3-byte instruction.
  - External interrupts were not the cause: raising them changed nothing.
- Now, over 250M instructions, 65.6 s emulated: main screen layout drawn, UI dispatcher
  running, 6 sector erases and 196608 words programmed, UART1 sends MIDI, and UART2 sends
  849 bytes to the Slave. These look like MIDI: `B0 7B 00` etc., All Notes Off per channel.
- **Task names** from RAM: PRECORD, PPLAY, PKBD, PSWITCH, PHDISPL, PADC, PPANEL, PDISPLAY,
  PNULL. Queues: QPANEL, QKBD, QDISPLAY, QHDISPL, QRECORD, QPLAY.
- **Font missing.** The text routine reads its table at FF0002 (F0851F). No supplied file
  covers FF0000-FF9FFF, so screens show boxes without text. That 32 KB flash sector is
  probably factory-resident, like the Slave boot. ob12.clearalgo.com lists nothing newer.
- From sheet 04/15: CS2 LED- From sheet 04/15: CS2 LED 68FF80-8F, CS3 LCD 68FF00-01 (A0 = C/D), CS4 keyboard 0000C0-CF.
- RAM at EC0000-EFFFFF is the 256 KB SRAM seen through a mirror (A18, A19 unconnected).

Core changes against upstream MAME, all in `src/f2mc16`:
- Generic effective-address decoder `ob12_ea.inc`: upstream lacks many addressing modes.
- Added NCC, CMR, CALLV, MOVX A,dir, MOVX A,io, XCH Ri,ea, MOVW @AL,AH and ALU forms.
- Bug fixed: interrupt level narrowed to 8 bits, clearing ILM and RP in PS on every interrupt.
- Bug fixed: `CALLP @RWx+disp8` return address.

## Slave boot, `src/ob12_slave.cpp` (measured)

A replacement stub stands in for the missing resident boot: banks EF, SP FFF0, ILM 7, JMPP
FC0500. Those values, the vector mapping, the UART2 enable and interrupt levels 2 and 3 are
choices based on the firmware, not known hardware values.

Over 120M instructions, 42.9 s emulated, replaying the 849 bytes captured from the Master:
- Prints `Hello, those fellows!`, `Slave Ver: Dec 11 2000`, and its polyphony line.
- Consumes all 849 bytes and **answers the Master**: 18 bytes, `11 "Dec 11 2000 1.11" 00`.
- ISRs sit on fixed 0x100 boundaries: FC0100 UART2 RX, FC0200 UART2 TX, FC0300 not yet
  identified, main at FC0500. Vectors live in the resident boot.
- Resident calls: FFD12E(0,2), FFD12E(1,4), FFD1BD(0), FFD215(0,5), FFD215(1,3), FFD215(2,0).
  Meaning unknown. The firmware never configures UART2, so these probably do.
- UART1 sends 16 bytes to the DAC side: `20 10 60 C2 80 FF C0 FF` twice.

DSP host protocol, read from FC2690-FC2790 and checked against the captured stream:
- Bytes go to 68FF80, and DASP selection is active low on port 7 bits 0-4.
- Load: command `0C`, the 12 header bytes, then PMEM words high byte first, padded to 512.
- The stream holds program 5 first, then program 1. PMEM in the stream equals the carved
  `prog1.pmem.bin` byte for byte. CMEM follows, also matching at its start.
- P60 EMPTY is modelled low and P61 RDY high; real timing is unknown.
- DASP4 stream keeps growing (49160 bytes) as the loop runs: runtime CMEM updates, format
  not read yet.

## Whole instrument, `tools/tms_harness/ob12_system.cpp` (measured)

Master, Slave and five DSP models in one process, UART2 linked live, stable for 30 s
emulated. The Master sends 849 bytes and the Slave answers `Dec 11 2000 1.11` at about 12 s.

Runtime host protocol, decoded and in use: `03` then 5-byte groups, 9-bit CMEM address and
24-bit value, ended by NDCS changing. Load: `0C` + 12 + 2048, then `05` + 1536.

**First sound.** Note-on bytes injected straight into the Slave, bypassing the Master:
- Voices are allocated three per DSP: 8 notes went 3, 3, 2 to DASP0, 1, 2. So 12 voices.
- About 50 CMEM writes per note.
- The voice bus into DASP4 carries a sawtooth: 196.0 Hz with harmonics 1, 1/2, 1/3, 1/4,
  1/5, 1/6. Signal is on the left channel of the odd half-frame slot only.

Not understood or not working:
- Note 60 sounds at 196 Hz, not 261.6 Hz. Patch transposition or a fault, unknown.
- Level keeps rising for as long as notes hold, with no note-off sent. Unexplained.
- Channel 0 alone gave silence; the signal appears with the next channels.
- A MIDI note sent to the Master is received but not forwarded to the Slave.
- DASP4 output now passes signal, with a large fault: see below.
- Secondary 0x34 is a noise generator per the public IDA module, 3 uses per voice DSP.
  It, 1FC, 0x3E flag 8 and 0x43 MACLO remain unimplemented in the local emulator.

## DASP4 output (measured)

Why it was silent, found with `tools/tms_harness/dasp4_trace.cpp`, a differential trace:
- Program 4 stores input at D00/D04 and reads D03, D0B, D0F, D11. No word writes those
  addresses directly: they are the same data some samples later through circular DMEM.
- Word 000 sets LDMEM, so word 001, the circular step, never advanced DOFF in the emulator.
- Hypothesis, applied in the local emulator copy only: a lock set by secondary 0x2E does not
  yet cover the next instruction. With it the signal spreads to over 200 state items and
  outputs open. It fits the program but is not checked against silicon.
- Half-frame phase: DASP4 reads the voice bus at D0D0/D0D1 in one half frame and at D08/D0C
  in the other. D08/D0C went nowhere in this run. The harness now offsets DASP4 by 256 steps.
  Real chip start phase is unknown.

Result with notes sent to the Slave, 20 s emulated:
- MOUT and AOUT carry signal on both channels, peaks near full scale.
- **Noise fault:** output starts 0.06 s after DSP start with no note played. RMS about 5400 of
  32768, spectrally flat (0.72), the same before and after notes. Cause unknown.
- The note is present under it: peaks 187.5, 375, 562.5, 750 Hz and a second series at 179,
  366.5, 554, 741.5 Hz. The voice bus earlier read 196 Hz; the change is unexplained.

Noise investigation, later round. The fault changed form but is not solved:
- The program never sets DCIRC, so the emulator's window swept all 256 DMEM cells and hit
  the fixed variables. Direct addresses split cleanly: unlocked 00-CC, locked CE-FF.
- Local change: a true ring over the low cells, fixed cells above. Ring size unknown; CC to D0
  were tried, none is clearly right. 100 (the old behaviour) is far worse.
- With the ring alone, silent input built up to full scale: unstable feedback.
- 0x43 as multiply by MACLO cut silent output from about 6,000,000 to 100,000. As load-and-
  halve (the IDA module's reading) it gave about 1,000,000. MACLO looks right, not proven.
- 1FC as written here is wrong or incomplete: it makes idle voices output DC (055C10).
  It is off for now, so 30 divide steps per sample still do nothing.
- Current output with ring CE, 0x43 on, 1FC off: broadband noise gone, spectral flatness
  0.72 to 0.0. Instead a full-scale impulse repeats at 233.0 Hz, 48000/206, the ring period.
  One cell or wrap point is wrong. Note level on the voice bus rises under it but is not
  audible beside the spike.
- All of these changes live only in the local emulator copy: 0x2E lock delay, ring, 0x43,
  1FC, audio port writes. They are listed here because that copy is not in this repo.

## Own DSP core, `src/tms57070` (measured)

Flat integer core, no external code at build time. The whole instrument now builds and runs
from this repo alone: `src/ob12_system.cpp`.

- `tools/tms_harness/core_compare.cpp`, options off, all five programs, 2000 frames each:
  5.12M steps identical in PC, ACC, MACC, CR1, XRD, DA, CA, DMEM, CMEM. About 20 ns per step.
  This proves agreement with the public emulator, not with silicon. Program coverage was
  with default coefficients, so paths needing live parameters were less exercised.
- **Licence:** this core is not clean-room. It was written after reading the public source,
  and its behaviour comes from that work, while the code and structure are our own. Whether
  that is enough is a judgement for Thor; asking the author remains the safe route.

**Noise and impulse: solved.** Programs 5 and 9 are start-up code run before the main load.
Program 9 ends `C70CD000`: DCIRC = CD, ring of 206 cells, as the address split predicted.
Program 5 sets DIR and CIR for the voice chips. The old harness never let them run.
With them run and the registers surviving the reload:
- Output is exactly silent with no note: 0 from the 0.06 s fault onward.
- Earlier noise and the 233 Hz impulse were harness faults, not DSP behaviour.
- The earlier 187.5 Hz and 179 Hz readings came from those faulty runs.

Options on in the system: audio ports, true ring, 2E lock delay, MACLO multiply. 1FC off.

Open, in order:
- MOUT and AOUT are silent even with notes, in both DASP4 phases. The voice bus into DASP4
  carries signal on left, odd half frames, rising to RMS 111015. Right is zero. Phase 1 puts
  that slot on D0D0; phase 0 puts it on D08. Superseded by "Two-slot audio bus": phase 0
  is the default and D08 does reach the output.
- The gain path needs finding: C77 and other coefficients are zero. They may come from a
  Master message not yet sent, for example panel or volume values.
- Unimplemented and hit every sample: 1FC, 3E flag 8, secondary 34.

## DASP4 gain study, `tools/dasp4_gain.cpp` (measured, own core)

- With a true ring the tone reaches D08, D0A, D0D, D0E, then D12 and D16 at 0F3/0F4. At
  0FD/0FE those are overwritten by products that carry no signal, so the coefficient behind
  0FB/0FC (read through CA) is zero, and the signal stops there.
- Setting all 300 zero CMEM words gives output, and a minimal set C00D, C00F, C0CB keeps it.
  **This does not show they are gains.** Forcing them to 200000 in the full system pins MOUT
  at full-scale DC with notes or without: an unstable feedback path, not an opened gain.
- Control messages: `F0 7F 30 04 <param> <hi> <lo> F7` is a real Slave command family, the
  Master sends four of them. Params 00-07 include one that writes C0CB = 03513E. No CC
  0-119 and no param 00-1F with value 04 7F wrote C00D or C00F. Output stayed silent.
- Slave parameter handling is table driven around FCD7xx. Not reversed yet.
- Voice pitch re-measured in the corrected run: 196.0 Hz sawtooth, so that reading stands.
  Note 60 should be 261.6 Hz; the patch may transpose, unknown. Level still rises while held.

**Solved: the outputs needed panel values.** The Master sends them only on change, and the
harness A/D was constant. With every pot moving from 14 s on (`OB12_POT_SWEEP`):
- Master to Slave grows from 849 to 18069 bytes: `A0`, `A8`, `B0`, `E0` messages and
  `F0 7F 30 04 01 00 5B F7`, about 1700 bytes per second.
- CMEM writes rise: DASP4 8948 to 61751, DASP0 3 to 37524.
- MOUT left carries the note: 196.0 Hz sawtooth, RMS about 200, peak 1397 of 32768, with no
  forced coefficient. MOUT right and AOUT stay zero. Sound is present on the real output.
- C00D, C00F stay 0 throughout, so they were never output gains.
- Not measured: which pot drives which parameter. The sweep moves all at once, so the level
  here is arbitrary and DASP0's output changes as parameters move.

## MIDI through the Master (measured)

- The UART1 receive ISR F0C75D stores bytes in a ring at EC5779 and ends `MOV I:9F,#1`. That
  is DIRR, the delayed interrupt module, vector 2A. Bytes are parsed there. It was not
  modelled; added to Master and Slave.
- With it, notes on 16 channels gave one forwarded message: `94 3C 64` at stream offset 12B4.
  The other 15 were not forwarded, which fits a single receive channel. Which input channel
  passed is not measured, though channel 0 was sent first and writes began at once.
- Master turns MIDI channel into Slave channel 4. DASP0 then takes the voice: CMEM writes rise
  from 3 to 37091.
- That single note is silent, and so are two. See "Voice slots" below: the path works.

Silent timbre study:
- The Master loads four timbres, `F0 7E 00..03`, and program data `F0 7E 04`.
- `tools/compare_channels.py`: Slave channel 4 writes the same values as channel 0. So the
  Master plays timbre 0. Timbres 1 and 2 differ in about 30 CMEM values, for example C014,
  C04B, C087. **Retracted:** timbre 0 is not silent, see "Two-slot audio bus" below.
- Noise hypothesis tested and rejected: enabling secondary 34 changed nothing.
- Voice code runs two divides of 16 steps by D04: 1F8 then 15 x 1FC, at 046 and 059.
- 1FC with a zero divisor made idle voices output DC, so a guard treats divisor 0 as no
  subtract. This guard is a hypothesis.
- With 1FC on, direct notes: the level is now steady, RMS 160510 on the bus instead of rising,
  so the rise was caused by the missing division. A second bus slot carries signal, RMS
  about 150000. Spectrum changed: 392 Hz strongest, 196 Hz at 0.36. 1FC is on by default.
- Right channel still zero.

## Two-slot audio bus (measured)

**Retraction.** "Timbre 0 keeps D04 at 0" was an inference and it is false. Measured live on
DASP0, D04 peaks at 00F755 for timbre 0. Reading `dmem[4]` is also wrong: D04 is cell
`(4 + DOFF) mod (DCIRC + 1)`. Replaying `dsp_stream_N.bin` alone gave DCIRC 0 and wrong CMEM,
so that replay is not a valid measurement; the probes now run inside `ob12_system`.

What D04 is: a scratch divisor, rewritten several times per sample. In one traced sample:
- 03D stores M1: 000061 for timbre 0, 000000 for timbre 1.
- 039 reads C04B into M0 as 00003C, 03C shifts it to 00000F, 040 stores that in D04.
- 046 clears it.

C04B is a mailbox: the host writes it, and 03A overwrites it with 00000F within a sample
(`OB12_WATCH_C=4B`). The program serves one table slot at a time: 020 loads CA from ACC, 44 in
the traced sample and 45 in the first. So the trace covers one slot only.

C04A: the DSP's first pass stores 0 over the loaded 02200E, at 036, because CA is 04A then.
Timbre 1 gets 02400E from the host later, timbre 0 does not. Holding C04A at 02200E left the
audio identical, read back as 02200E, so this does not explain the timbre difference.

Bus structure, from the programs:
- Each program has two slots, 000-0FF and 100-1FF, and each slot writes and reads the ports.
  That is four bus channels per sample: slot A L/R and slot B L/R.
- prog1 forwards D02/D03 in slot A and D00/D01 in slot B. prog0 clears them instead of reading.
- DASP4 reads slot A into D0D0/D0D1 and slot B into D008/D00C. It never reads AR2.
- Control words run on the AX3 to AR2 chain, D05/D06.

**Fixed: a word must stay in its slot from chip to chip.** A one half frame hop swapped slots
at every chip, so DASP0's D00 landed in DASP1's D02 and no sum formed. OB12_PHASE=1 had been
hiding this. Three notes on channels 0-2, pots moving, MOUT left RMS at 21 s:

| Hop | Phase | MOUT left | Bus |
|---|---|---|---|
| 1 | 0 | 24768 | slots mixed |
| 1 | 1 | 73591 | slots mixed |
| 2 | 0 | 82349 | sum grows 43368 to 224627 in slot B; slot A constant 116 |
| 2 | 1 | 38 | same bus, DASP4 on the wrong slot |

Hop 2, phase 0 are now the defaults: all five chips in step, no phase offset. Hop 2 is chosen
because the firmware's summing bus needs it. Serial timing on the board is not measured.
The earlier "DASP1 and DASP3 output 82" was an artefact of sampling odd half frames only.

**Caveat on this table.** Direct injection is not one note, see "Voice slots". It shows what
each Slave channel does with several notes, not a single-note result.

One note injected per Slave channel, hop 2, phase 0, note at 16 s, pots moving from 14 s:

| Slave channel | DASP4 AX2 L RMS | Sound |
|---|---|---|
| 0, 4 | 14331 | noise-like, flatness 0.84, 16-bit RMS about 82; CMEM written continuously |
| 1, 5 | 140403 | clean sawtooth, 196 Hz, harmonics 1, 0.51, 0.34, 0.26, 0.21 |
| 2, 3, 6 | 0 | silent |

Idle with no note: MOUT exactly 0 for 22 s. Right channel, AOUT and AX2 R stay 0; D01 and D03
are 0 on every chip, so the voices never write the right bus.

Slot A's constant is not audio. M0 holds 000074 at 1FE and D02 holds the same value; with one
note both are 000090. The `00021002` at 003 stores into D02 rather than clearing it.

## Voice slots (measured)

**The Master path works.** The Master's stream and the direct stream differ only at the note:

    MIDI:   ... 10 5e  94 3c 64  a0 3c 65 35 7f 2a 5f b0 ...
    direct: ... 10 5e  (94 3c 64 injected)  3c 65 35 7f 2a 5f b0 ...

The Master resends status `A0` after a note. My injection does not, so the pot data after it
can read as more notes under running status. That fits DASP1 receiving writes only in direct
runs. It is inferred from the bytes, the Slave's parser was not traced.

Notes sent through the Master with `OB12_MIDI_NOTES`, 0.5 s apart, from 16 s:

| Notes | Result |
|---|---|
| 3C; 37; 2A; 3C at 18.2 s | silent, except 37: RMS 78, 1500 Hz multiples, not studied |
| 3C,3C; 3C,40 | silent |
| 3C,40,43; 3C,35,2A | sound starts at 17.0 s, the third note; noise-like, flatness 0.84 |

So sound needs the third voice of DASP0. It is voice slot, not time: a single note at 18.2 s
is also silent.

The three voice blocks, stride 6B from 10B, are not the same code. Slots 0 and 1 have a stage
at 11C-12B and 187-196, using C1C9/C1CA and cells D04E-50, D05F-61. Slot 2 has none there; its
words at 04A-062 use C1CB. The signal stops at that stage: 186 holds M0 FEDBBF, 187 reads 0.
With one note M0 is 02C5B3 at 11B and 0 at 11C, with C1C9 = 200000. The class 2 word `9002ABC9`
reads D04E, and D04E-50 stay 0, so the stage has no input that I can find. Slot 0 output is
then 0 with one note, FFFFA2 with three; slot 2 gives 008991.

Rejected: secondary 34 as noise, retested with one note on the fixed bus, levels unchanged.
Its real meaning remains unknown, and it runs just before this stage in all three slots.

Open:
- What feeds D04E-50. Candidates: secondary 34 storing to DB0, secondary 3E flag 8.
- Timbre 0 being noise-like: the patch, or an emulation fault.
- Whether the 196 Hz reading came from another note than 60. Direct runs may have played
  extra notes, so note 60 has never been measured alone.

## Tooling and licences

- MAME `src/devices/cpu/f2mc16` includes an `MB90611A` device, BSD-3-Clause. Some
  opcodes only log "Unimplemented" (MULU, DIVU, CALLV and others).
- `Prehistoricman/TMS57070` has no licence file, so its code must not be copied.
  Get permission or write our own core. Local measurement runs only, allowed by Thor;
  nothing from it is in this repo. `tools/tms_harness/harness.cpp` is our own driver and
  needs a local copy of that emulator to build.
- `ioelectro/fujitsu-dev` holds Softune-style MB90 installers and an ISP example, no
  licence shown. Not installed. Possible later source of register headers or a simulator.
