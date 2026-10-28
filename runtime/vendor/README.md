`gba-core` and `armv4t` from https://github.com/JRickey/gba-recomp at
de4edf59b872d887046d6a3b005e2df551b6d44c (MIT OR Apache-2.0, LICENSE-MIT).

Patched for a PS5 title, which forbids TLS: `#![no_std]` + `alloc`, every `std::`
path routed to `src/nostd.rs` (trace env vars read as unset, debug prints compile
away). Fix: MSR writing CPSR's T bit now switches ARM/Thumb as hardware does
(`exec_msr` in gba-core/src/exec.rs; Pokémon Pinball R&S's unpacker needs it,
upstream masked T and the game stayed on a black screen; worth sending upstream).
Fix: the cartridge RTC (`bit_pos` in gba-core/src/rtc.rs) moves data bytes LSB
first whatever order the command came in, as the S-3511 and mGBA do; upstream
followed the command's order, so Pokémon Ruby/Sapphire/Emerald read bit-reversed
dates and reported "the internal battery has run dry". Also worth sending upstream.
Saves: `runtime/overrides.tsv` (derived from mGBA's override table, MPL-2.0) is
consulted before the ROM's library strings for the save type and the RTC
(`backup::override_for`); SRAM is 32 KB mirrored across its 64 KB window as on the
cartridge (64 KB only for `sram64` rows), and a 512-byte EEPROM saves as 512 bytes,
so game.sav files match other emulators' .sav.
Save states: serde derives on Machine, Cpu, MemMap (+Timer, Dma, VPhase), Apu, Flash,
Eeprom, Rtc and their enums; `#[serde(skip)]` on ROM, BIOS, the decode cache, audio_buf,
the HLE sound engines (mp2k/gax/rdrv, never armed by gba2ps5) and their taps.
Generated C from gba-recomp must come from the same commit: it depends on
the `repr(C)` layout of `Machine`/`Cpu`.
