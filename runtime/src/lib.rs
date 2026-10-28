//! C ABI over gba-core for the PS5 frontend (no_std: a PS5 title forbids TLS).
#![no_std]
extern crate alloc;

use alloc::boxed::Box;
use core::alloc::{GlobalAlloc, Layout};
use gba_core::machine::Machine;

extern "C" {
    fn aligned_alloc(align: usize, size: usize) -> *mut u8;
    fn free(p: *mut u8);
    fn abort() -> !;
}

struct Libc;
unsafe impl GlobalAlloc for Libc {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        // aligned_alloc wants size to be a multiple of align
        aligned_alloc(l.align(), (l.size() + l.align() - 1) & !(l.align() - 1))
    }
    unsafe fn dealloc(&self, p: *mut u8, _: Layout) {
        free(p)
    }
}
#[global_allocator]
static A: Libc = Libc;

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    unsafe { abort() }
}

// Stable's prebuilt core/alloc still name this even with panic = "abort"; nothing unwinds.
#[no_mangle]
extern "C" fn rust_eh_personality() {}

/// New machine (HLE BIOS) from a ROM image; the bytes are copied.
#[no_mangle]
pub unsafe extern "C" fn gba_new(rom: *const u8, len: usize) -> *mut Machine {
    Box::into_raw(Box::new(Machine::new(core::slice::from_raw_parts(rom, len).to_vec())))
}

/// Same, booting a real BIOS (16 KB, the player's own dump) from reset: the GBA intro plays and
/// BIOS calls run the real code instead of the HLE.
#[no_mangle]
pub unsafe extern "C" fn gba_new_bios(rom: *const u8, len: usize, bios: *const u8, bios_len: usize) -> *mut Machine {
    let rom = core::slice::from_raw_parts(rom, len).to_vec();
    Box::into_raw(Box::new(Machine::new_with_bios(rom, core::slice::from_raw_parts(bios, bios_len))))
}

#[no_mangle]
pub unsafe extern "C" fn gba_free(m: *mut Machine) {
    drop(Box::from_raw(m))
}

/// Run one frame with `keys` (GBA KEYINPUT bits, 1 = pressed). Returns the 240x160 BGR555 framebuffer.
#[no_mangle]
pub unsafe extern "C" fn gba_frame(m: *mut Machine, keys: u16) -> *const u16 {
    let m = &mut *m;
    m.bus.keys = !keys & 0x3ff; // hardware is active-low
    m.run_frame(1 << 22);
    m.bus.framebuffer.as_ptr()
}

/// Move up to `max` interleaved stereo samples (65536 Hz) into `out`; returns how many.
#[no_mangle]
pub unsafe extern "C" fn gba_audio(m: *mut Machine, out: *mut i16, max: usize) -> usize {
    let buf = &mut (*m).bus.audio_buf;
    let n = buf.len().min(max);
    core::ptr::copy_nonoverlapping(buf.as_ptr(), out, n);
    buf.drain(..n);
    n
}

/// Battery save: pointer + length, or len 0 when the cart has none.
#[no_mangle]
pub unsafe extern "C" fn gba_save(m: *const Machine, len: *mut usize) -> *const u8 {
    match (*m).bus.save_data() {
        Some(s) => { *len = s.len(); s.as_ptr() }
        None => { *len = 0; core::ptr::null() }
    }
}

#[no_mangle]
pub unsafe extern "C" fn gba_load_save(m: *mut Machine, data: *const u8, len: usize) {
    (*m).bus.load_save_data(core::slice::from_raw_parts(data, len))
}

/// Cheats: work RAM by GBA address (0x02xxxxxx EWRAM, 0x03xxxxxx IWRAM, mirrored as on the bus).
/// Anything else is ignored: codes only touch RAM, never ROM or I/O.
unsafe fn ram(m: *mut Machine, addr: u32) -> Option<&'static mut [u8]> {
    let bus = &mut (*m).bus;
    let (mem, off) = match addr >> 24 {
        0x02 => (&mut bus.ewram, (addr & 0x3_FFFF) as usize),
        0x03 => (&mut bus.iwram, (addr & 0x7FFF) as usize),
        _ => return None,
    };
    Some(&mut core::slice::from_raw_parts_mut(mem.as_mut_ptr(), mem.len())[off..])
}

#[no_mangle]
pub unsafe extern "C" fn gba_peek16(m: *mut Machine, addr: u32) -> u16 {
    match ram(m, addr & !1) {
        Some(r) if r.len() >= 2 => u16::from_le_bytes([r[0], r[1]]),
        _ => 0,
    }
}

#[no_mangle]
pub unsafe extern "C" fn gba_poke16(m: *mut Machine, addr: u32, v: u16) {
    if let Some(r) = ram(m, addr & !1) {
        if r.len() >= 2 {
            r[..2].copy_from_slice(&v.to_le_bytes());
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn gba_poke8(m: *mut Machine, addr: u32, v: u8) {
    if let Some(r) = ram(m, addr) {
        r[0] = v;
    }
}

/// Save states: the whole machine (CPU, RAM, video, sound, timers, DMA, backup, clock) as postcard
/// bytes after a version word; ROM, BIOS and the decode cache stay out (they come from the running
/// machine). Bump STATE_VERSION when a saved gba-core struct changes, so old states are refused.
const STATE_VERSION: u32 = 2; // 2: cartridge devices (solar, gyro, rumble, tilt) in the state

/// Snapshot into a new buffer (free it with gba_state_free); NULL if it failed.
#[no_mangle]
pub unsafe extern "C" fn gba_state_save(m: *const Machine, len: *mut usize) -> *mut u8 {
    let mut out = alloc::vec::Vec::from(STATE_VERSION.to_le_bytes());
    match postcard::to_extend(&*m, out) {
        Ok(v) => out = v,
        Err(_) => return core::ptr::null_mut(),
    }
    *len = out.len();
    alloc::boxed::Box::into_raw(out.into_boxed_slice()) as *mut u8
}

#[no_mangle]
pub unsafe extern "C" fn gba_state_free(p: *mut u8, len: usize) {
    if !p.is_null() {
        drop(alloc::boxed::Box::from_raw(core::ptr::slice_from_raw_parts_mut(p, len)));
    }
}

/// Replace the machine's state with a snapshot. 0 = loaded; -1 = not a state of this version
/// (the machine is left untouched). The framebuffer moves: take it from the next gba_frame.
#[no_mangle]
pub unsafe extern "C" fn gba_state_load(m: *mut Machine, data: *const u8, len: usize) -> i32 {
    let bytes = core::slice::from_raw_parts(data, len);
    if len < 4 || bytes[..4] != STATE_VERSION.to_le_bytes() {
        return -1;
    }
    let Ok(mut new) = postcard::from_bytes::<Machine>(&bytes[4..]) else { return -1 };
    let old = &mut *m;
    new.bus.rom = core::mem::take(&mut old.bus.rom);
    new.bus.bios = core::mem::take(&mut old.bus.bios);
    *old = new;
    0
}

/// Cartridge extras: 1 clock, 2 solar sensor, 4 gyro, 8 rumble, 16 tilt.
#[no_mangle]
pub unsafe extern "C" fn gba_hw(m: *const Machine) -> u32 {
    let bus = &(*m).bus;
    let mut f = 0;
    if let Some(r) = &bus.rtc {
        f |= r.has_clock as u32 | (r.light as u32) << 1 | (r.gyro as u32) << 2 | (r.rumble as u32) << 3;
    }
    f | (bus.tilt.is_some() as u32) << 4
}

/// Sensor inputs from the controller, read by the game when it samples: rotation rate and tilt in
/// sensor units around their centres, and sunlight 0 (dark) ..= 255.
#[no_mangle]
pub unsafe extern "C" fn gba_hw_input(m: *mut Machine, gyro: i32, tilt_x: i32, tilt_y: i32, light: u8) {
    let bus = &mut (*m).bus;
    if let Some(r) = &mut bus.rtc {
        r.gyro_input = gyro;
        r.light_level = light;
    }
    if let Some(t) = &mut bus.tilt {
        t.input_x = tilt_x;
        t.input_y = tilt_y;
    }
}

/// How hard the rumble motor ran since the last call, 0 ..= 255 (share of pin writes with it on;
/// with no writes, as it was last left).
#[no_mangle]
pub unsafe extern "C" fn gba_hw_rumble(m: *mut Machine) -> u8 {
    let Some(r) = &mut (*m).bus.rtc else { return 0 };
    let duty = if r.rumble_writes > 0 { r.rumble_on * 255 / r.rumble_writes } else { r.rumble_last as u32 * 255 };
    r.rumble_writes = 0;
    r.rumble_on = 0;
    duty as u8
}

/// What the cartridge saves to and whether it has a clock, for the log: e.g. "flash128+rtc".
#[no_mangle]
pub unsafe extern "C" fn gba_save_kind(m: *const Machine) -> *const u8 {
    use gba_core::backup::BackupKind::*;
    let rtc = (*m).bus.rtc.as_ref().is_some_and(|r| r.has_clock);  // the GPIO port may hold only sensors
    let s: &[u8] = match ((*m).bus.backup, rtc) {
        (None, false) => b"none\0",
        (None, true) => b"none+rtc\0",
        (Sram, false) => b"sram\0",
        (Sram, true) => b"sram+rtc\0",
        (Flash64, false) => b"flash64\0",
        (Flash64, true) => b"flash64+rtc\0",
        (Flash128, false) => b"flash128\0",
        (Flash128, true) => b"flash128+rtc\0",
        (Eeprom, false) => b"eeprom\0",
        (Eeprom, true) => b"eeprom+rtc\0",
    };
    s.as_ptr()
}

/// RetroAchievements' GBA address space (rcheevos consoleinfo.c): 0x00000 IWRAM (32 KB),
/// 0x08000 EWRAM (256 KB), 0x48000 cartridge save (64 KB). Copies up to `n` bytes from `addr`;
/// returns how many exist there (0 = unmapped), as rc_client's read_memory wants.
#[no_mangle]
pub unsafe extern "C" fn gba_ra_read(m: *const Machine, addr: u32, out: *mut u8, n: u32) -> u32 {
    let bus = &(*m).bus;
    let (mem, off): (&[u8], usize) = match addr {
        0..=0x7FFF => (&bus.iwram, addr as usize),
        0x8000..=0x4_7FFF => (&bus.ewram, addr as usize - 0x8000),
        0x4_8000..=0x5_7FFF => (bus.save_data().unwrap_or(&[]), addr as usize - 0x4_8000),
        _ => return 0,
    };
    if off >= mem.len() {
        return 0;
    }
    let k = (n as usize).min(mem.len() - off);
    core::ptr::copy_nonoverlapping(mem.as_ptr().add(off), out, k);
    k as u32
}

