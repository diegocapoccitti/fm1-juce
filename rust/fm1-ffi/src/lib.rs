//! C interface to the emulated FM-1 for the JUCE plugin.
//!
//! The machine's thread (`engine.rs`) and the portable saves (`savefile.rs`) are fm1-plugin's own
//! files, compiled here unchanged: this crate only gives them the parameters they read and puts
//! the plugin's resampler and its boot / load / export logic behind `extern "C"` functions
//! (Source/Fm1Bridge.h declares them).
//!
//! Threads: `fm1_process` and `fm1_midi` belong to the audio thread, `fm1_view`, `fm1_key`,
//! `fm1_enc` to the GUI, `fm1_load_file` / `fm1_export_file` block and belong to a background
//! thread; the rest may be called from anywhere.

use serde::{Deserialize, Serialize};
use std::ffi::{c_char, CStr};
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Mutex};

#[path = "../../../external/fm1-emu/crates/fm1-plugin/src/engine.rs"]
pub mod engine;
#[path = "../../../external/fm1-emu/crates/fm1-plugin/src/savefile.rs"]
pub mod savefile;

use engine::{Cmd, Midi, Shared, View, RATE};

// ------------------------------------------------------------ what engine.rs reads --

/// The firmware package, saved with the project.
#[derive(Clone, Default, Serialize, Deserialize)]
pub struct Firmware {
    pub name: String,
    #[serde(with = "b64")]
    pub pkg: Arc<Vec<u8>>,
}

/// A flash sector the firmware wrote, saved with the project.
#[derive(Clone, Serialize, Deserialize)]
pub struct Sector {
    pub at: u32,
    #[serde(with = "b64")]
    pub data: Arc<Vec<u8>>,
}

#[derive(Clone, Copy)]
pub struct Clock(u64);
impl Clock {
    pub fn mhz(self) -> u64 { self.0 }
}

pub struct ClockParam(AtomicU64);
impl ClockParam {
    pub fn value(&self) -> Clock { Clock(self.0.load(Ordering::Relaxed)) }
}

pub struct FloatParam(AtomicU32);
impl FloatParam {
    pub fn value(&self) -> f32 { f32::from_bits(self.0.load(Ordering::Relaxed)) }
    fn set(&self, v: f32) { self.0.store(v.clamp(0.0, 1.0).to_bits(), Ordering::Relaxed) }
}

/// The parameters the machine's thread reads (nih-plug's `Fm1Params` in fm1-plugin).
pub struct Fm1Params {
    pub firmware: Mutex<Firmware>,
    pub flash: Mutex<Vec<Sector>>,
    pub master: FloatParam,
    pub clock: ClockParam,
}

impl Default for Fm1Params {
    fn default() -> Self {
        Fm1Params {
            firmware: Mutex::new(Firmware::default()),
            flash: Mutex::new(Vec::new()),
            master: FloatParam(AtomicU32::new((900.0f32 / 1023.0).to_bits())),
            clock: ClockParam(AtomicU64::new(240)),
        }
    }
}

// ------------------------------------------------------------------- the handle --

/// The audio thread's side: rings and the resampler (fm1-plugin's `process`).
struct Audio {
    midi: rtrb::Producer<Midi>,
    out: rtrb::Consumer<[f32; 2]>,
    ratio: f64,
    lead: usize,
    taken: u64,
    frac: f64,
    cur: [f32; 2],
    next: [f32; 2],
}

pub struct Fm1 {
    params: Arc<Fm1Params>,
    shared: Arc<Shared>,
    cmds: Mutex<mpsc::Sender<Cmd>>,
    /// the package the machine runs (to boot only when the project's is another)
    running: Mutex<Option<Arc<Vec<u8>>>>,
    audio: Mutex<Audio>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl Fm1 {
    fn send(&self, c: Cmd) -> bool {
        self.cmds.lock().map(|tx| tx.send(c).is_ok()).unwrap_or(false)
    }

    /// Boot the project's firmware unless the machine already runs it.
    fn boot_project(&self) {
        let fw = self.params.firmware.lock().unwrap().clone();
        let sectors = self.params.flash.lock().unwrap().clone();
        let mut running = self.running.lock().unwrap();
        if fw.pkg.is_empty() || running.as_ref().is_some_and(|r| Arc::ptr_eq(r, &fw.pkg) || **r == *fw.pkg) {
            return;
        }
        *running = Some(fw.pkg.clone());
        let sectors = sectors.iter().map(|s| (s.at, s.data.to_vec())).collect();
        self.send(Cmd::Boot { pkg: fw.pkg.clone(), sectors });
    }

    fn load(&self, path: &std::path::Path) -> Result<String, String> {
        let current = self.params.firmware.lock().unwrap().clone();
        let data = savefile::read_with_firmware(path, Some(&current))?;
        let firmware = data.firmware.clone();
        let (tx, rx) = mpsc::channel();
        if !self.send(Cmd::Load { firmware: data.firmware, sectors: data.sectors, reply: tx }) {
            return Err("the machine stopped".into());
        }
        rx.recv().map_err(|_| "the machine stopped while loading".to_owned())??;
        *self.running.lock().unwrap() = Some(firmware.pkg);
        Ok(format!("Loaded {}", firmware.name))
    }

    fn export(&self, path: &std::path::Path) -> Result<String, String> {
        let mut path = path.to_path_buf();
        if path.extension().is_none() { path.set_extension("fm1save"); }
        let firmware = self.params.firmware.lock().unwrap().clone();
        if firmware.pkg.is_empty() { return Err("load a firmware first".into()); }
        let (tx, rx) = mpsc::channel();
        if !self.send(Cmd::Snapshot { pkg: firmware.pkg.clone(), reply: tx }) {
            return Err("the machine stopped".into());
        }
        let sectors = rx.recv().map_err(|_| "the machine stopped while exporting".to_owned())??;
        savefile::write(&path, &savefile::SaveData { firmware, sectors })?;
        Ok(format!("Exported {}", path.file_name().unwrap_or_default().to_string_lossy()))
    }
}

impl Drop for Fm1 {
    fn drop(&mut self) {
        self.shared.quit.store(true, Ordering::Relaxed);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

fn h<'a>(p: *const Fm1) -> Option<&'a Fm1> {
    unsafe { p.as_ref() }
}

/// Copy `s` into a C buffer of `cap` bytes, NUL-terminated (cut at a char boundary).
fn write_str(s: &str, buf: *mut c_char, cap: usize) {
    if buf.is_null() || cap == 0 { return; }
    let mut n = s.len().min(cap - 1);
    while !s.is_char_boundary(n) { n -= 1; }
    unsafe {
        std::ptr::copy_nonoverlapping(s.as_ptr(), buf as *mut u8, n);
        *buf.add(n) = 0;
    }
}

fn path_of(p: *const c_char) -> Option<std::path::PathBuf> {
    if p.is_null() { return None; }
    unsafe { CStr::from_ptr(p) }.to_str().ok().map(std::path::PathBuf::from)
}

// --------------------------------------------------------------------- the C API --

/// What the editor shows besides the screen and the LEDs.
#[repr(C)]
pub struct Fm1Status {
    pub booted: i32,
    pub halted: i32,
    /// 0 FM-1, 1 SMK-37
    pub board: i32,
    pub speed: f32,
    pub late: u64,
}

#[no_mangle]
pub extern "C" fn fm1_new() -> *mut Fm1 {
    let shared = Arc::new(Shared {
        view: Mutex::new(View {
            lcd: vec![0; 240 * 240],
            leds: [0.0; 41],
            pads: [0; 16],
            board: fm1_soc::Board::Fm1,
            booted: false,
            halted: false,
            speed: 0.0,
            starved: 0,
            late: 0,
        }),
        quit: Default::default(),
    });
    let params = Arc::new(Fm1Params::default());
    let (tx, rx) = mpsc::channel();
    let (midi_tx, midi_rx) = rtrb::RingBuffer::new(4096);
    let (audio_tx, audio_rx) = rtrb::RingBuffer::new(16384);
    let (sh, pa) = (shared.clone(), params.clone());
    let thread = std::thread::Builder::new()
        .name("fm1-machine".into())
        .spawn(move || engine::run(sh, pa, rx, midi_rx, audio_tx))
        .ok();
    let audio = Audio { midi: midi_tx, out: audio_rx, ratio: 1.0, lead: 2048, taken: 0, frac: 0.0, cur: [0.0; 2], next: [0.0; 2] };
    Box::into_raw(Box::new(Fm1 { params, shared, cmds: Mutex::new(tx), running: Mutex::new(None), audio: Mutex::new(audio), thread }))
}

#[no_mangle]
pub extern "C" fn fm1_free(p: *mut Fm1) {
    if !p.is_null() {
        drop(unsafe { Box::from_raw(p) });
    }
}

/// The host's rate and block size; returns the latency to report, in host samples.
#[no_mangle]
pub extern "C" fn fm1_prepare(p: *const Fm1, sample_rate: f64, max_block: i32) -> i32 {
    let Some(m) = h(p) else { return 0 };
    let latency = {
        let mut a = m.audio.lock().unwrap();
        a.ratio = RATE / sample_rate.max(1.0);
        // a host block's frames, and room for a DMA half and the emulation's unevenness
        a.lead = (max_block.max(1) as f64 * a.ratio).ceil() as usize + 1024;
        m.send(Cmd::Lead(a.lead));
        (a.lead as f64 / a.ratio).round() as i32
    };
    m.boot_project();
    latency
}

/// A channel message at `offset` samples into the block about to be processed.
#[no_mangle]
pub extern "C" fn fm1_midi(p: *const Fm1, offset: i32, b0: u8, b1: u8, b2: u8, len: u8) {
    let Some(m) = h(p) else { return };
    let Ok(mut a) = m.audio.try_lock() else { return };
    let frame = a.taken + (offset.max(0) as f64 * a.ratio) as u64 + a.lead as u64;
    let _ = a.midi.push(Midi { frame, bytes: [b0, b1, b2], len: len.min(3) });
}

/// Fill `n` samples of each channel with the machine's output, resampled to the host's rate.
#[no_mangle]
pub extern "C" fn fm1_process(p: *const Fm1, left: *mut f32, right: *mut f32, n: i32) {
    let Some(m) = h(p) else { return };
    if left.is_null() || right.is_null() || n <= 0 { return; }
    let (l, r) = unsafe { (std::slice::from_raw_parts_mut(left, n as usize), std::slice::from_raw_parts_mut(right, n as usize)) };
    let Ok(mut guard) = m.audio.try_lock() else {
        l.fill(0.0);
        r.fill(0.0);
        return;
    };
    let a = &mut *guard;
    let mut starved = 0u64;
    for i in 0..n as usize {
        a.frac += a.ratio;
        while a.frac >= 1.0 {
            match a.out.pop() {
                Ok(f) => {
                    a.cur = a.next;
                    a.next = f;
                    a.taken += 1;
                    a.frac -= 1.0;
                }
                Err(_) => {
                    // the machine is behind: hold, and take the frame next time
                    a.frac = a.frac.min(1.0);
                    starved += 1;
                    break;
                }
            }
        }
        let t = a.frac.min(1.0) as f32;
        l[i] = a.cur[0] + (a.next[0] - a.cur[0]) * t;
        r[i] = a.cur[1] + (a.next[1] - a.cur[1]) * t;
    }
    if starved > 0 {
        if let Ok(mut v) = m.shared.view.try_lock() {
            v.starved += starved;
        }
    }
}

#[no_mangle]
pub extern "C" fn fm1_key(p: *const Fm1, id: i32, down: i32) {
    if let Some(m) = h(p) {
        if (0..64).contains(&id) {
            m.send(Cmd::Key { id: id as u8, down: down != 0 });
        }
    }
}

/// Turn matrix encoder `enc` by `clicks` detents (+ clockwise).
#[no_mangle]
pub extern "C" fn fm1_enc(p: *const Fm1, enc: i32, clicks: i32) {
    if let Some(m) = h(p) {
        m.send(Cmd::Enc { enc: enc.clamp(0, 255) as u8, clicks });
    }
}

/// The MASTER pot, 0..1.
#[no_mangle]
pub extern "C" fn fm1_set_master(p: *const Fm1, v: f32) {
    if let Some(m) = h(p) { m.params.master.set(v); }
}

/// The emulated CPU clock in MHz; changing it reboots the firmware with its flash kept.
#[no_mangle]
pub extern "C" fn fm1_set_clock(p: *const Fm1, mhz: i32) {
    if let Some(m) = h(p) {
        m.params.clock.0.store(mhz.clamp(60, 480) as u64, Ordering::Relaxed);
    }
}

/// The screen as 240x240 0xAARRGGBB pixels, the 41 LEDs (0..1 duty), and the status.
#[no_mangle]
pub extern "C" fn fm1_view(p: *const Fm1, argb: *mut u32, leds: *mut f32, status: *mut Fm1Status) {
    let Some(m) = h(p) else { return };
    let Ok(v) = m.shared.view.lock() else { return };
    if !argb.is_null() {
        let out = unsafe { std::slice::from_raw_parts_mut(argb, 240 * 240) };
        for (o, &px) in out.iter_mut().zip(v.lcd.iter()) {
            let px = px as u32;
            let (r, g, b) = ((px >> 11) * 255 / 31, ((px >> 5) & 63) * 255 / 63, (px & 31) * 255 / 31);
            *o = 0xff00_0000 | r << 16 | g << 8 | b;
        }
    }
    if !leds.is_null() {
        unsafe { std::ptr::copy_nonoverlapping(v.leds.as_ptr(), leds, 41) };
    }
    if let Some(s) = unsafe { status.as_mut() } {
        s.booted = v.booted as i32;
        s.halted = v.halted as i32;
        s.board = (v.board == fm1_soc::Board::Smk37) as i32;
        s.speed = v.speed;
        s.late = v.late;
    }
}

/// The running firmware's name ("" when none).
#[no_mangle]
pub extern "C" fn fm1_firmware_name(p: *const Fm1, buf: *mut c_char, cap: usize) {
    let name = h(p).map(|m| m.params.firmware.lock().unwrap().name.clone()).unwrap_or_default();
    write_str(&name, buf, cap);
}

#[no_mangle]
pub extern "C" fn fm1_has_firmware(p: *const Fm1) -> i32 {
    h(p).is_some_and(|m| !m.params.firmware.lock().unwrap().pkg.is_empty()) as i32
}

/// Load a .fwsc package or a .fm1save setup. Blocks until the machine booted it (or refused):
/// call from a background thread. Returns 1 on success; `msg` gets the status line either way.
#[no_mangle]
pub extern "C" fn fm1_load_file(p: *const Fm1, path: *const c_char, msg: *mut c_char, cap: usize) -> i32 {
    let (Some(m), Some(path)) = (h(p), path_of(path)) else { return 0 };
    let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| m.load(&path)))
        .unwrap_or_else(|_| Err("internal error".into()));
    let ok = r.is_ok();
    write_str(&r.unwrap_or_else(|e| format!("Load: {e}")), msg, cap);
    ok as i32
}

/// Export the running firmware with its live flash as a .fm1save. Blocks: background thread.
#[no_mangle]
pub extern "C" fn fm1_export_file(p: *const Fm1, path: *const c_char, msg: *mut c_char, cap: usize) -> i32 {
    let (Some(m), Some(path)) = (h(p), path_of(path)) else { return 0 };
    let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| m.export(&path)))
        .unwrap_or_else(|_| Err("internal error".into()));
    let ok = r.is_ok();
    write_str(&r.unwrap_or_else(|e| format!("Export: {e}")), msg, cap);
    ok as i32
}

/// A buffer the library made (the project state); free it with `fm1_buf_free`.
#[repr(C)]
pub struct Fm1Buf {
    pub data: *mut u8,
    pub len: usize,
}

/// The project state: the firmware and its saved flash, as a portable .fm1save (empty: none).
#[no_mangle]
pub extern "C" fn fm1_get_state(p: *const Fm1) -> Fm1Buf {
    let empty = Fm1Buf { data: std::ptr::null_mut(), len: 0 };
    let Some(m) = h(p) else { return empty };
    let firmware = m.params.firmware.lock().unwrap().clone();
    if firmware.pkg.is_empty() { return empty; }
    let sectors = m.params.flash.lock().unwrap().clone();
    match savefile::encode(&savefile::SaveData { firmware, sectors }) {
        Ok(bytes) => {
            let mut b = bytes.into_boxed_slice();
            let out = Fm1Buf { data: b.as_mut_ptr(), len: b.len() };
            std::mem::forget(b);
            out
        }
        Err(_) => empty,
    }
}

#[no_mangle]
pub extern "C" fn fm1_buf_free(b: Fm1Buf) {
    if !b.data.is_null() {
        drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(b.data, b.len)) });
    }
}

/// Restore a project state from `fm1_get_state` and boot it. Returns 1 if it was valid.
#[no_mangle]
pub extern "C" fn fm1_set_state(p: *const Fm1, data: *const u8, len: usize) -> i32 {
    let Some(m) = h(p) else { return 0 };
    if data.is_null() || len == 0 { return 0; }
    let bytes = unsafe { std::slice::from_raw_parts(data, len) };
    let Ok(save) = savefile::decode(bytes) else { return 0 };
    *m.params.firmware.lock().unwrap() = save.firmware;
    *m.params.flash.lock().unwrap() = save.sectors;
    m.boot_project();
    1
}

/// Bytes as base64 in the saved state.
mod b64 {
    use base64::Engine;
    use serde::{Deserialize, Deserializer, Serializer};
    use std::sync::Arc;

    pub fn serialize<S: Serializer>(v: &Arc<Vec<u8>>, s: S) -> Result<S::Ok, S::Error> {
        s.serialize_str(&base64::engine::general_purpose::STANDARD.encode(v.as_slice()))
    }

    pub fn deserialize<'de, D: Deserializer<'de>>(d: D) -> Result<Arc<Vec<u8>>, D::Error> {
        let s = String::deserialize(d)?;
        base64::engine::general_purpose::STANDARD.decode(s).map(Arc::new).map_err(serde::de::Error::custom)
    }
}
