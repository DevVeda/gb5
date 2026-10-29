//! gba2ps5: link cable, multiplayer mode (16-bit, 2-4 units), for N machines in one process.
//!
//! Registers (hardware reference, "SIO Multi-Player Mode"): SIOMULTI0-3 at 0x120-0x126 hold
//! every unit's word after a transfer, SIOCNT at 0x128 (bits 0-1 baud, 2 SI = child, 3 SD = all
//! ready, 4-5 player id, 7 start/busy, 12-13 = 0b10 multiplayer, 14 IRQ enable), SIOMLT_SEND at
//! 0x12A, RCNT bit 15 = 0 for SIO. Player 0 is the master: its start bit begins a transfer, the
//! others' words are latched, and after the baud-rate time every unit gets all words (0xFFFF for
//! empty slots), busy clears and the serial IRQ fires where enabled. Normal (8/32-bit) and UART
//! modes stay plain register storage.
//!
//! `Link` runs the machines in lockstep: every machine advances to the same group time in
//! slices, and a slice ends early at a transfer's completion so it lands on time everywhere.

#[allow(unused_imports)]
use crate::nostd::prelude::*;

use crate::machine::Machine;
use crate::mem::{irq, MemMap, CYCLES_PER_SCANLINE, SCANLINES_PER_FRAME};

const FRAME: u64 = CYCLES_PER_SCANLINE * SCANLINES_PER_FRAME;

/// Link cable state of one machine (not in save states).
#[derive(Default)]
pub struct Sio {
    /// Place in a linked group (0 = master); `None` = no cable, SIO is plain storage.
    pub player: Option<u8>,
    /// Master only: clock at which it started a transfer, picked up by `Link`.
    pub started: Option<u64>,
    pub busy: bool,
}

impl MemMap {
    pub(crate) fn sio_multi(&self) -> bool {
        self.io[0x135] & 0x80 == 0 && self.io[0x129] & 0x30 == 0x20
    }

    /// After a SIOCNT byte write: a linked master's start bit begins a transfer. Checked on both
    /// bytes because a halfword write stores the start bit before the mode bits.
    pub(crate) fn sio_write(&mut self) {
        let Some(p) = self.sio.player else { return };
        if self.io[0x128] & 0x80 == 0 || !self.sio_multi() {
            return;
        }
        self.io[0x128] &= !0x80; // reads back from `busy`
        if p == 0 && !self.sio.busy {
            self.sio.busy = true;
            self.sio.started = Some(self.clock);
        }
    }

    /// SIOCNT low byte while linked in multiplayer mode.
    pub(crate) fn sio_status(&self) -> u8 {
        let p = self.sio.player.unwrap_or(0);
        self.io[0x128] & 0x03 | ((p != 0) as u8) << 2 | 1 << 3 | p << 4 | (self.sio.busy as u8) << 7
    }

    fn sio_send(&self) -> u16 {
        u16::from_le_bytes([self.io[0x12A], self.io[0x12B]])
    }

    fn sio_finish(&mut self, data: &[u16; 4]) {
        for (i, w) in data.iter().enumerate() {
            self.io[0x120 + 2 * i..0x122 + 2 * i].copy_from_slice(&w.to_le_bytes());
        }
        self.sio.busy = false;
        if self.io[0x129] & 0x40 != 0 {
            self.raise_irq(irq::SERIAL);
        }
    }
}

/// Master cycles for one transfer: per unit a start bit, 16 data bits, a stop bit and a short
/// gap (~20 bit times), plus the master's lead-in. Within 1% of what mGBA measured.
pub fn transfer_cycles(siocnt: u8, units: usize) -> u64 {
    let baud = [9600, 38400, 57600, 115200][siocnt as usize & 3];
    (units as u64 * 20 + 2) * (1u64 << 24) / baud
}

pub struct Link {
    pub machines: Vec<Box<Machine>>,
    /// Each machine's clock at group time 0.
    base: Vec<u64>,
    now: u64,
    /// Transfer in flight: group time it completes, and the words.
    pending: Option<(u64, [u16; 4])>,
    /// Lockstep granularity in cycles: the most a slave's latched word can lag the master's start.
    pub slice: u64,
}

impl Link {
    /// Link 2-4 machines; machine i is player i (0 = master). Machines that are mid-frame
    /// relative to player 0 run alone until their video timing matches, so one `frame` call
    /// gives every machine a whole picture.
    pub fn new(mut machines: Vec<Box<Machine>>) -> Link {
        assert!((2..=4).contains(&machines.len()));
        let phase = machines[0].bus.clock % FRAME;
        for (i, m) in machines.iter_mut().enumerate() {
            let target = m.bus.clock + (phase + FRAME - m.bus.clock % FRAME) % FRAME;
            while m.bus.clock < target {
                m.step();
            }
            m.bus.sio = Sio { player: Some(i as u8), ..Default::default() };
        }
        let base = machines.iter().map(|m| m.bus.clock).collect();
        Link { machines, base, now: 0, pending: None, slice: 512 }
    }

    /// Unlink: every machine keeps running alone.
    pub fn into_machines(mut self) -> Vec<Box<Machine>> {
        for m in &mut self.machines {
            m.bus.sio = Sio::default();
        }
        self.machines
    }

    /// Run until player 0 completes a frame (all machines share its video phase).
    pub fn frame(&mut self) {
        for m in &mut self.machines {
            m.bus.frame_ready = false;
        }
        while !self.machines[0].bus.frame_ready {
            self.run_slice();
        }
    }

    pub fn run_slice(&mut self) {
        let mut stop = self.now + self.slice;
        if let Some((done, _)) = self.pending {
            stop = stop.min(done.max(self.now));
        }
        for (m, &b) in self.machines.iter_mut().zip(&self.base) {
            let end = b + stop;
            while m.bus.clock < end {
                if m.bus.halted && !m.bus.irq_line() {
                    // Sleep in event-sized hops that stop at the slice end (`step` would jump
                    // to the next event, possibly past it).
                    let hop = (m.bus.next_event.max(m.bus.clock + 1)).min(end) - m.bus.clock;
                    m.bus.tick(hop);
                } else {
                    m.step();
                }
            }
        }
        self.now = stop;
        if let Some((done, data)) = self.pending {
            if done <= stop {
                self.pending = None;
                for m in &mut self.machines {
                    m.bus.sio_finish(&data);
                }
            }
        }
        if self.pending.is_none() {
            if let Some(c) = self.machines[0].bus.sio.started.take() {
                // ponytail: words latched at the slice end, not at the master's start instant
                // (lag <= `slice`); fine while games set SIOMLT_SEND well before the next start.
                let mut data = [0xFFFF; 4];
                for (d, m) in data.iter_mut().zip(&self.machines) {
                    *d = m.bus.sio_send();
                }
                for m in &mut self.machines[1..] {
                    m.bus.sio.busy = true;
                }
                let n = self.machines.len();
                let done = c - self.base[0] + transfer_cycles(self.machines[0].bus.io[0x128], n);
                self.pending = Some((done, data));
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bus::Bus;

    fn parked() -> Box<Machine> {
        let mut m = Box::new(Machine::new(vec![0; 64]));
        m.bus.rom[0..4].copy_from_slice(&0xEAFF_FFFEu32.to_le_bytes()); // b .
        m
    }

    fn multi(m: &mut Machine, send: u16) {
        m.bus.write16(0x0400_0134, 0); // RCNT: SIO
        m.bus.write16(0x0400_0128, 0x6003); // multiplayer, 115200, IRQ on
        m.bus.write16(0x0400_012A, send);
    }

    #[test]
    fn unlinked_siocnt_is_storage() {
        let mut m = parked();
        multi(&mut m, 1);
        m.bus.write16(0x0400_0128, 0x6083);
        assert_eq!(m.bus.read16(0x0400_0128), 0x6083);
        assert_eq!(m.bus.sio.started, None);
    }

    #[test]
    fn master_transfer_reaches_every_unit() {
        let mut l = Link::new(vec![parked(), parked(), parked()]);
        for (i, m) in l.machines.iter_mut().enumerate() {
            multi(m, 0x1111 * (i as u16 + 1));
        }
        // Slave reads: child, all ready, its id, not busy. A slave's start bit does nothing.
        assert_eq!(l.machines[2].bus.read16(0x0400_0128), 0x6003 | 4 | 8 | 2 << 4);
        l.machines[1].bus.write16(0x0400_0128, 0x6083);
        assert_eq!(l.machines[1].bus.sio.started, None);
        assert_eq!(l.machines[0].bus.read16(0x0400_0128), 0x6003 | 8);

        let t0 = l.machines[0].bus.clock;
        l.machines[0].bus.write16(0x0400_0128, 0x6083);
        assert_eq!(l.machines[0].bus.read16(0x0400_0128) & 0x80, 0x80, "master busy");
        let mut done_at = 0;
        for _ in 0..100 {
            l.run_slice();
            if l.machines[0].bus.read16(0x0400_0128) & 0x80 == 0 {
                done_at = l.machines[0].bus.clock;
                break;
            }
            assert_eq!(l.machines[2].bus.read16(0x0400_0128) & 0x80, 0x80, "slave busy");
        }
        let want = transfer_cycles(3, 3);
        assert!(done_at >= t0 + want && done_at < t0 + want + 64, "{} vs {}", done_at - t0, want);
        for m in &mut l.machines {
            let r: Vec<u16> = (0..4).map(|i| m.bus.read16(0x0400_0120 + 2 * i)).collect();
            assert_eq!(r, [0x1111, 0x2222, 0x3333, 0xFFFF]);
            assert!(m.bus.reg_if & irq::SERIAL != 0);
            assert_eq!(m.bus.read16(0x0400_0128) & 0x80, 0);
        }
    }

    #[test]
    fn halted_slave_wakes_on_serial_irq() {
        let mut l = Link::new(vec![parked(), parked()]);
        multi(&mut l.machines[0], 7);
        multi(&mut l.machines[1], 9);
        l.machines[1].bus.write16(0x0400_0200, irq::SERIAL);
        l.machines[1].bus.halted = true;
        l.machines[0].bus.write16(0x0400_0128, 0x6083);
        for _ in 0..40 {
            l.run_slice();
        }
        assert!(!l.machines[1].bus.halted || l.machines[1].bus.irq_line());
        assert_eq!(l.machines[1].bus.read16(0x0400_0120), 7);
    }

    #[test]
    fn link_aligns_video_phase() {
        let mut a = parked();
        let mut b = parked();
        for _ in 0..12345 {
            a.step();
        }
        b.step();
        let l = Link::new(vec![a, b]);
        let (a, b) = (l.machines[0].bus.clock % FRAME, l.machines[1].bus.clock % FRAME);
        assert!(a.abs_diff(b) < 16, "{a} vs {b}");
    }
}
