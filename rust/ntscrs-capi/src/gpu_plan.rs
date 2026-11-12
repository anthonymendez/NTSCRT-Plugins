//! GPU plan exports (NTSCRT-Plugins, docs/dev/NTSC-GPU.md).
//!
//! The GPU port of the ntsc-rs effect (core/stages/ntsc/gpu/) runs the pass
//! list of `NtscEffect::apply_effect_to_yiq_field` (vendor/ntsc-rs/crates/
//! ntscrs/src/ntsc.rs) as compute kernels. Everything that is random, 64-bit
//! or depends on f64 / transcendental functions is resolved here, on the CPU,
//! with ntsc-rs's own code paths, and handed to the GPU as small tables:
//!
//!   - the "plan": which passes run, in order, with their IIR coefficients
//!     (exactly as ntsc-rs builds them), initial-condition factors and delays;
//!   - a per-plane-row table (`NtscGpuRow`): source image row (fielding),
//!     field membership, chroma subcarrier phase, per-row chroma phase noise
//!     rotation, chroma-loss flag;
//!   - a per-output-row table (`NtscGpuOutRow`): the Bob deinterlace;
//!   - per-row noise seeds (`NtscGpuRowSeed`, for phase 2's noise passes).
//!
//! ntsc-rs keeps its filter / RNG helpers private (`mod filter`, `mod random`,
//! `pub(crate) VHSTapeSpeed::filter_params`), so the minimal pieces are copied
//! below, each with the source lines it reproduces. The copies are Rust
//! compiled by the same toolchain with the same operations in the same order,
//! so their f32 results are bit-identical to ntsc-rs's. vendor/ is never
//! modified.
//!
//! The debug export `ntsc_debug_process_yiq` runs ntsc-rs itself on a frame
//! with a subset of its passes and returns the YIQ planes (before
//! deinterlacing), for pass-by-pass comparison with the GPU.

use std::panic::{catch_unwind, AssertUnwindSafe};

use rand::{Rng, RngCore, SeedableRng};
use rand_xoshiro::Xoshiro256PlusPlus;
use siphasher::sip::SipHasher;
use std::hash::{Hash, Hasher};

use ntsc_rs::settings::standard::{
    ChromaDemodulationFilter, ChromaLowpass, FbmNoiseSettings, FilterType, LumaLowpass, PhaseShift, VHSTapeSpeed,
};
use ntsc_rs::yiq_fielding::{BlitInfo, Bgrx, Rgbx, YiqField, YiqView};
use ntsc_rs::NtscEffect;

use fearless_simd::Level;

use crate::gpu_noise::{sample_noise_1d, sample_noise_2d, Fbm, Simplex, Simplex1d, Simplex2d};
use crate::{effect_for, NtscInstance, NTSC_BGRA8, NTSC_RGBA8};

// ---- stages (bit numbers), in apply_effect_to_yiq_field order ----
pub const ST_LUMA_FILTER: u32 = 0;
pub const ST_CHROMA_LOWPASS_IN: u32 = 1;
pub const ST_CHROMA_INTO_LUMA: u32 = 2; // always runs in ntsc-rs
pub const ST_COMPOSITE_SHARPENING: u32 = 3;
pub const ST_COMPOSITE_NOISE: u32 = 4;
pub const ST_SNOW: u32 = 5;
pub const ST_HEAD_SWITCHING: u32 = 6;
pub const ST_TRACKING_NOISE: u32 = 7;
pub const ST_LUMA_INTO_CHROMA: u32 = 8; // always runs in ntsc-rs
pub const ST_LUMA_SMEAR: u32 = 9;
pub const ST_RINGING: u32 = 10;
pub const ST_LUMA_NOISE: u32 = 11;
pub const ST_CHROMA_NOISE: u32 = 12;
pub const ST_CHROMA_PHASE_ERROR: u32 = 13;
pub const ST_CHROMA_PHASE_NOISE: u32 = 14;
pub const ST_CHROMA_DELAY: u32 = 15;
pub const ST_EDGE_WAVE: u32 = 16;
pub const ST_TAPE_SPEED: u32 = 17;
pub const ST_CHROMA_LOSS: u32 = 18;
pub const ST_SHARPEN: u32 = 19;
pub const ST_VERT_BLEND: u32 = 20;
pub const ST_CHROMA_LOWPASS_OUT: u32 = 21;
#[allow(dead_code)]
pub const ST_COUNT: u32 = 22;

/// Stages the GPU implements: all of them (phases 1-3). What still keeps a
/// frame on ntsc-rs is outside the stage list (Screen Loop, Metal, software
/// devices) or the one degenerate case ntsc_gpu_plan marks itself
/// (chroma_vert_blend on a field of one row, where ntsc-rs panics).
pub const GPU_IMPLEMENTED: u32 = (1 << ST_COUNT) - 1;

// ---- pass kinds ----
pub const K_FILTER: i32 = 1; // row IIR on the planes in `planes` (filter[p] per plane)
pub const K_LUMA_BOX: i32 = 2; // input luma box filter (serial per row)
pub const K_CHROMA_INTO_LUMA: i32 = 3; // modulate I/Q into Y (row table xi)
pub const K_DEMOD: i32 = 4; // luma_into_chroma; mode = ChromaDemodulationFilter, filter[0] = notch (mode 1)
pub const K_ROTATE_CONST: i32 = 5; // rotate (I, Q) by a = sin, b = cos
pub const K_ROTATE_ROWS: i32 = 6; // rotate (I, Q) by the row table's sin / cos
pub const K_CHROMA_LOSS: i32 = 7; // zero I, Q on rows flagged in the row table
pub const K_VERT_BLEND: i32 = 8; // chroma_vert_blend, per field
pub const K_NOISE: i32 = 9; // video_noise_line on `planes`: mode = seed pass, a = frequency, b = intensity * 0.25, pad[0] = octaves
pub const K_SPECKLES: i32 = 10; // add the stage's speckle events (ntsc_gpu_speckles) to Y
pub const K_SHIFT_ROWS: i32 = 11; // shift_row on `planes` by the stage's row table (ntsc_gpu_row_shifts)
pub const K_CHROMA_DELAY: i32 = 12; // chroma_delay: a = horizontal shift, mode = vertical offset, pad[0] = 1: copy rows

/// `ntsc_gpu_row_seeds` pass of tracking noise's noise lines: its seeder is
/// `Seeder(seed).mix(TRACKING_NOISE).mix(frame).mix(2)` and its rows are the
/// affected rows with index `index + cut_off_rows` (ntsc.rs 887-935).
pub const SEEDS_TRACKING: u32 = noise_seeds::TRACKING_NOISE as u32;

/// NtscGpuRowShift::boundary of a row the pass leaves alone.
pub const SHIFT_NONE: i32 = -1;

pub const ROW_FLAG_CHROMA_LOSS: u32 = 1;

pub const INIT_ZERO: i32 = 0;
pub const INIT_FIRST_SAMPLE: i32 = 1;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NtscGpuFilter {
    pub num: [f32; 4],
    pub den: [f32; 4], // without the leading 1; den[3] is always 0
    pub zi: [f32; 4],  // z[k] = zi[k] * initial value (initial_condition_into(1.0))
    pub len: i32,      // coefficients incl. the denominator's implicit 1 (1..=4); 0 = no filter
    pub delay: i32,
    pub initial: i32, // INIT_ZERO / INIT_FIRST_SAMPLE
    pub initial_value: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NtscGpuPass {
    pub stage: i32,
    pub kind: i32,
    pub planes: u32, // bit 0 Y, 1 I, 2 Q
    pub mode: i32,
    pub a: f32,
    pub b: f32,
    pub pad: [i32; 2],
    pub filter: [NtscGpuFilter; 3], // per plane (Y, I, Q)
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NtscGpuRow {
    pub src_row: u32,     // source image row this plane row is read from
    pub field_row: u32,   // index of the row in its field (ntsc-rs's per-row `index`)
    pub field_first: u32, // plane row of the field's first row
    pub field_rows: u32,  // rows in the field
    pub xi: u32,          // chroma_phase_shift(...) of the row (its field's frame number)
    pub flags: u32,       // ROW_FLAG_*
    pub phase_sin: f32,   // chroma phase noise rotation of the row
    pub phase_cos: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NtscGpuOutRow {
    pub a: u32, // plane rows averaged for this output row (a == b: copied)
    pub b: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NtscGpuPlanInfo {
    pub seed: u64,             // CommonInfo::seed
    pub field_frame: [u64; 2], // frame number apply_effect_to_yiq_field gets, per field
    pub field: i32,            // 0 Upper, 1 Lower, 2 Both, 3 InterleavedUpper, 4 InterleavedLower
    pub plane_rows: u32,
    pub num_fields: u32,
    pub field_first: [u32; 2],
    pub field_rows: [u32; 2],
    pub num_passes: u32,
    pub unsupported: u32, // stages that are on but not implemented on the GPU
    pub enabled: u32,     // stages that run (after `stage_mask`)
    pub horizontal_scale: f32,
    pub vertical_scale: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NtscGpuRowSeed {
    pub seed_lo: u32, // Seeder(seed).mix(pass).mix(frame).mix(field_row).finalize::<u64>()
    pub seed_hi: u32,
    pub noise_seed: i32, // video_noise_line: rng.next_u32() as i32 (Fbm seed)
    pub offset: f32,     // video_noise_line: rng.random::<f32>() * width
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NtscGpuRowShift {
    pub shift: f32,      // horizontal shift in pixels (shift_row semantics)
    pub boundary: i32,   // 0 Constant(0.0), 1 Extend, SHIFT_NONE (-1) = row untouched
    pub copy_start: i32, // head-switching mid line: first pixel taken from the shifted row; -1 = whole row
    pub intensity: f32,  // head switching: the mid line's transient intensity (its values are speckles);
                         // tracking noise: the row's noise-line intensity (already * 0.25)
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NtscGpuSpeckle {
    pub row: u32,          // plane row
    pub x0: i32,           // first pixel (already clamped to the row)
    pub count: u32,        // pixels
    pub value_offset: u32, // index of the first of `count` additive values in the values array
}

// ---- Copied helpers ----

/// Copy of ntsc-rs random.rs `Seeder` (lines 62-83).
#[derive(Clone)]
struct Seeder {
    state: SipHasher,
}
impl Seeder {
    fn new<T: Hash>(seed: T) -> Self {
        let mut hasher = SipHasher::new_with_keys(0, 0);
        seed.hash(&mut hasher);
        Seeder { state: hasher }
    }
    fn mix<T: Hash>(mut self, input: T) -> Self {
        input.hash(&mut self.state);
        self
    }
    fn finalize_u64(self) -> u64 {
        self.state.finish()
    }
    // random.rs FromSeeder for i32 (lines 40-45)
    fn finalize_i32(self) -> i32 {
        (self.state.finish() & (u32::MAX as u64)) as i32
    }
    // random.rs FromSeeder for f32 (lines 54-59)
    fn finalize_f32(self) -> f32 {
        let input = self.state.finish();
        f32::from_bits(((input & 0xFFFFFFFF) as u32 >> 9) | 0x3F800000) - 1.0
    }
}

/// Copy of ntsc-rs random.rs `Geometric` (lines 5-28).
struct Geometric {
    lambda: f64,
}
impl Geometric {
    fn new(p: f64) -> Self {
        if p <= 0.0 || p > 1.0 {
            panic!("Invalid probability: {p}");
        }
        Geometric { lambda: (1.0 - p).ln() }
    }
}
impl rand::distr::Distribution<usize> for Geometric {
    fn sample<R: rand::Rng + ?Sized>(&self, rng: &mut R) -> usize {
        (rng.random::<f64>().ln() / self.lambda) as usize
    }
}

/// ntsc.rs noise_seeds (lines 591-604): the `pass` values of ntsc_gpu_row_seeds.
#[allow(dead_code)]
pub mod noise_seeds {
    pub const VIDEO_COMPOSITE: u64 = 0;
    pub const HEAD_SWITCHING: u64 = 2;
    pub const TRACKING_NOISE: u64 = 3;
    pub const VIDEO_CHROMA_PHASE: u64 = 4;
    pub const EDGE_WAVE: u64 = 5;
    pub const SNOW: u64 = 6;
    pub const CHROMA_LOSS: u64 = 7;
    pub const HEAD_SWITCHING_MID_LINE_JITTER: u64 = 8;
    pub const VIDEO_CHROMA_I: u64 = 1;
    pub const VIDEO_CHROMA_Q: u64 = 9;
    pub const VIDEO_LUMA: u64 = 10;
}

/// Copy of ntsc-rs filter.rs `TransferFunction` construction (lines 5-151, 378-406):
/// the coefficient math only (the filtering itself runs on the GPU).
#[derive(Clone)]
struct Tf {
    coeffs: [f32; 7],
    len: usize,
}

fn polynomial_multiply(a: &[f32], b: &[f32], dst: &mut [f32]) -> usize {
    let degree = a.len() + b.len() - 1;
    for ai in 0..a.len() {
        for bi in 0..b.len() {
            dst[ai + bi] += a[ai] * b[bi];
        }
    }
    degree
}

fn trim_zeros(mut input: &[f32]) -> &[f32] {
    while input.last().is_some_and(|last| *last == 0.0) {
        input = &input[..input.len() - 1]
    }
    input
}

impl Tf {
    fn new(num: &[f32], den: &[f32]) -> Self {
        assert!(num.len() <= 4 && den.len() <= 3, "Filter is too large.");
        let len = den.len() + 1;
        assert!(num.len() <= len, "Numerator length exceeds denominator length.");
        let mut coeffs = [0f32; 7];
        let (dst_num, dst_den) = coeffs.split_at_mut(4);
        dst_num[..num.len()].copy_from_slice(num);
        dst_den[..den.len()].copy_from_slice(den);
        Tf { coeffs, len }
    }

    fn with_scale(&self, scale: f32) -> Self {
        let (num, den) = self.coeffs.split_at(4);
        let mut new_num = [0f32; 4];
        new_num[0] = scale * num[0] + (1.0 - scale);
        for i in 1..self.len {
            new_num[i] = scale * num[i] + (1.0 - scale) * den[i - 1];
        }
        Tf::new(&new_num[..self.len], &den[..self.len - 1])
    }

    fn cascade_self(&self, n: usize) -> Self {
        let mut filt = self.clone();
        for _ in 1..n {
            filt = filt.mul(self);
        }
        filt
    }

    fn num_den(&self) -> (&[f32], &[f32]) {
        (&self.coeffs[..self.len], &self.coeffs[4..4 + self.len - 1])
    }

    fn mul(&self, rhs: &Tf) -> Tf {
        let (lhs_num, lhs_den) = self.num_den();
        let (rhs_num, rhs_den) = rhs.num_den();
        let mut lhs_den_full = [0f32; 4];
        lhs_den_full[0] = 1.0;
        lhs_den_full[1..1 + lhs_den.len()].copy_from_slice(lhs_den);
        let mut rhs_den_full = [0f32; 4];
        rhs_den_full[0] = 1.0;
        rhs_den_full[1..1 + rhs_den.len()].copy_from_slice(rhs_den);
        let mut num = [0f32; 7];
        let mut den = [0f32; 7];
        polynomial_multiply(lhs_num, rhs_num, &mut num);
        polynomial_multiply(&lhs_den_full[..self.len], &rhs_den_full[..rhs.len], &mut den);
        let num = trim_zeros(&num);
        let den = &trim_zeros(&den)[1..];
        Tf::new(num, den)
    }

    // filter.rs initial_condition_into (lines 116-151)
    fn initial_condition_into(&self, value: f32, dst: &mut [f32]) {
        let filter_len = self.len;
        assert_eq!(dst.len(), filter_len);
        dst.fill(0.0);
        if value.abs() == 0.0 {
            return;
        }
        let (num, den) = self.num_den();
        let mut b_sum = 0.0;
        for i in 1..filter_len {
            let num_i = num.get(i).unwrap_or(&0.0);
            let den_i = den.get(i - 1).unwrap_or(&0.0);
            b_sum += num_i - den_i * num[0];
        }
        dst[0] = b_sum / (den.iter().sum::<f32>() + 1.0);
        let mut a_sum = 1.0;
        let mut c_sum = 0.0;
        for i in 1..filter_len - 1 {
            let num_i = num.get(i).unwrap_or(&0.0);
            let den_i = den.get(i - 1).unwrap_or(&0.0);
            a_sum += den_i;
            c_sum += num_i - den_i * num[0];
            dst[i] = (a_sum * dst[0] - c_sum) * value;
        }
        dst[0] *= value;
    }

    fn to_gpu(&self, initial: i32, delay: usize) -> NtscGpuFilter {
        let (num, den) = self.num_den();
        let mut f = NtscGpuFilter {
            len: self.len as i32,
            delay: delay as i32,
            initial,
            ..Default::default()
        };
        f.num[..num.len()].copy_from_slice(num);
        f.den[..den.len()].copy_from_slice(den);
        // With value 1.0 every `* value` is exact, so zi[k] * v on the GPU is
        // the same single rounding ntsc-rs does for the steady state v.
        let mut z = [0f32; 4];
        self.initial_condition_into(1.0, &mut z[..self.len]);
        f.zi = z;
        f
    }
}

// ntsc.rs lines 20-89
const NTSC_RATE: f32 = (315000000.00 / 88.0) * 4.0;
use std::f32::consts::{FRAC_1_SQRT_2, PI};

fn make_lowpass(cutoff: f32, rate: f32) -> Tf {
    let time_interval = 1.0 / rate;
    let tau = (cutoff * 2.0 * PI).recip();
    let alpha = time_interval / (tau + time_interval);
    Tf::new(&[alpha], &[-(1.0 - alpha)])
}
fn make_lowpass_triple(cutoff: f32, rate: f32) -> Tf {
    make_lowpass(cutoff, rate).cascade_self(3)
}
fn make_lowpass_for_type(cutoff: f32, rate: f32, filter_type: FilterType) -> Tf {
    match filter_type {
        FilterType::ConstantK => make_lowpass_triple(cutoff, rate),
        FilterType::Butterworth => make_butterworth_filter(cutoff, rate),
    }
}
fn make_notch_filter(freq: f32, quality: f32) -> Tf {
    if !(0.0..=1.0).contains(&freq) {
        panic!("Frequency outside valid range");
    }
    let bandwidth = (freq / quality) * PI;
    let freq = freq * PI;
    let beta = (bandwidth * 0.5).tan();
    let gain = (1.0 + beta).recip();
    let num = [gain, -2.0 * freq.cos() * gain, gain];
    let den = [-2.0 * freq.cos() * gain, 2.0 * gain - 1.0];
    Tf::new(&num, &den)
}
fn make_butterworth_filter(cutoff: f32, rate: f32) -> Tf {
    let freq = (2.0 * cutoff).min(rate) / rate;
    let freq = freq * PI;
    let (omega_s, omega_c) = freq.sin_cos();
    let q_value = FRAC_1_SQRT_2;
    let alpha = omega_s / (2.0 * q_value);
    let gain = (1.0 + alpha).recip();
    Tf::new(
        &[
            (1.0 - omega_c) * 0.5 * gain,
            (1.0 - omega_c) * gain,
            (1.0 - omega_c) * 0.5 * gain,
        ],
        &[-2.0 * omega_c * gain, (1.0 - alpha) * gain],
    )
}

// ntsc.rs chroma_phase_shift (lines 276-289)
fn chroma_phase_shift(scanline_phase_shift: PhaseShift, offset: i32, frame_num: usize, line_num: usize) -> usize {
    match scanline_phase_shift {
        PhaseShift::Degrees90 | PhaseShift::Degrees270 => {
            ((frame_num as i32 + offset + ((line_num as i32) >> 1)) & 3) as usize
        }
        PhaseShift::Degrees180 => ((((frame_num + line_num) & 2) as i32 + offset) & 3) as usize,
        PhaseShift::Degrees0 => 0,
    }
}

// settings/standard.rs VHSTapeSpeed::filter_params (lines 96-117, pub(crate) there)
struct TapeParams {
    luma_cut: f32,
    chroma_cut: f32,
    chroma_delay: usize,
}
fn tape_params(s: VHSTapeSpeed) -> Option<TapeParams> {
    match s {
        VHSTapeSpeed::NONE => None,
        VHSTapeSpeed::SP => Some(TapeParams { luma_cut: 2400000.0, chroma_cut: 320000.0, chroma_delay: 4 }),
        VHSTapeSpeed::LP => Some(TapeParams { luma_cut: 1900000.0, chroma_cut: 300000.0, chroma_delay: 5 }),
        VHSTapeSpeed::EP => Some(TapeParams { luma_cut: 1400000.0, chroma_cut: 280000.0, chroma_delay: 6 }),
    }
}

// ---- stage masking (debug: run only some passes in ntsc-rs itself) ----

/// Turns off every optional stage whose bit is clear in `mask`. The two
/// modulation stages always run in ntsc-rs (they cannot be turned off).
fn mask_effect(e: &mut NtscEffect, mask: u32) {
    let on = |s: u32| mask & (1 << s) != 0;
    if !on(ST_LUMA_FILTER) {
        e.input_luma_filter = LumaLowpass::None;
    }
    if !on(ST_CHROMA_LOWPASS_IN) {
        e.chroma_lowpass_in = ChromaLowpass::None;
    }
    if !on(ST_COMPOSITE_SHARPENING) {
        e.composite_sharpening = 0.0;
    }
    if !on(ST_COMPOSITE_NOISE) {
        e.composite_noise = None;
    }
    if !on(ST_SNOW) {
        e.snow_intensity = 0.0;
    }
    if !on(ST_HEAD_SWITCHING) {
        e.head_switching = None;
    }
    if !on(ST_TRACKING_NOISE) {
        e.tracking_noise = None;
    }
    if !on(ST_LUMA_SMEAR) {
        e.luma_smear = 0.0;
    }
    if !on(ST_RINGING) {
        e.ringing = None;
    }
    if !on(ST_LUMA_NOISE) {
        e.luma_noise = None;
    }
    if !on(ST_CHROMA_NOISE) {
        e.chroma_noise = None;
    }
    if !on(ST_CHROMA_PHASE_ERROR) {
        e.chroma_phase_error = 0.0;
    }
    if !on(ST_CHROMA_PHASE_NOISE) {
        e.chroma_phase_noise_intensity = 0.0;
    }
    if !on(ST_CHROMA_DELAY) {
        e.chroma_delay_horizontal = 0.0;
        e.chroma_delay_vertical = 0;
    }
    if let Some(v) = e.vhs_settings.as_mut() {
        if !on(ST_EDGE_WAVE) {
            v.edge_wave = None;
        }
        if !on(ST_TAPE_SPEED) {
            // Also turns sharpening off: ntsc-rs sharpens with the tape speed's luma cut.
            v.tape_speed = VHSTapeSpeed::NONE;
        }
        if !on(ST_CHROMA_LOSS) {
            v.chroma_loss = 0.0;
        }
        if !on(ST_SHARPEN) {
            v.sharpen = None;
        }
    }
    if !on(ST_VERT_BLEND) {
        e.chroma_vert_blend = false;
    }
    if !on(ST_CHROMA_LOWPASS_OUT) {
        e.chroma_lowpass_out = ChromaLowpass::None;
    }
}

/// Which stages run with these settings.
fn enabled_stages(e: &NtscEffect, horizontal_scale: f32) -> u32 {
    let mut m = (1 << ST_CHROMA_INTO_LUMA) | (1 << ST_LUMA_INTO_CHROMA);
    let mut set = |s: u32, c: bool| {
        if c {
            m |= 1 << s;
        }
    };
    set(ST_LUMA_FILTER, e.input_luma_filter != LumaLowpass::None);
    set(ST_CHROMA_LOWPASS_IN, e.chroma_lowpass_in != ChromaLowpass::None);
    set(ST_COMPOSITE_SHARPENING, e.composite_sharpening != 0.0);
    set(ST_COMPOSITE_NOISE, e.composite_noise.is_some());
    set(ST_SNOW, e.snow_intensity > 0.0 && horizontal_scale > 0.0);
    set(ST_HEAD_SWITCHING, e.head_switching.is_some());
    set(ST_TRACKING_NOISE, e.tracking_noise.is_some());
    set(ST_LUMA_SMEAR, e.luma_smear > 0.0);
    set(ST_RINGING, e.ringing.is_some());
    set(ST_LUMA_NOISE, e.luma_noise.is_some());
    set(ST_CHROMA_NOISE, e.chroma_noise.is_some());
    set(ST_CHROMA_PHASE_ERROR, e.chroma_phase_error > 0.0);
    set(ST_CHROMA_PHASE_NOISE, e.chroma_phase_noise_intensity > 0.0);
    set(ST_CHROMA_DELAY, e.chroma_delay_horizontal != 0.0 || e.chroma_delay_vertical != 0);
    if let Some(v) = &e.vhs_settings {
        set(ST_EDGE_WAVE, v.edge_wave.as_ref().is_some_and(|w| w.intensity > 0.0));
        let tape = tape_params(v.tape_speed).is_some();
        set(ST_TAPE_SPEED, tape);
        set(ST_CHROMA_LOSS, v.chroma_loss > 0.0);
        set(ST_SHARPEN, v.sharpen.is_some() && tape);
    }
    set(ST_VERT_BLEND, e.chroma_vert_blend);
    set(ST_CHROMA_LOWPASS_OUT, e.chroma_lowpass_out != ChromaLowpass::None);
    m
}

/// apply_effect_to_yiq_field's CommonInfo (ntsc.rs lines 1156-1186).
fn common_info(e: &NtscEffect, height: usize, scale_factor: [f32; 2]) -> (u64, f32, f32) {
    let seed = e.random_seed as u32 as u64;
    let scale_factor = scale_factor.map(|scale_factor| {
        scale_factor
            * if e.scale.as_ref().is_some_and(|scale| scale.scale_with_video_size) {
                height as f32 / 480.0
            } else {
                1.0
            }
    });
    let h = e.scale.as_ref().map(|s| s.horizontal_scale * scale_factor[0]).unwrap_or(1.0);
    let v = e.scale.as_ref().map(|s| s.vertical_scale * scale_factor[1]).unwrap_or(1.0);
    (seed, h, v)
}

fn field_code(f: YiqField) -> i32 {
    match f {
        YiqField::Upper => 0,
        YiqField::Lower => 1,
        YiqField::Both => 2,
        YiqField::InterleavedUpper => 3,
        YiqField::InterleavedLower => 4,
    }
}

/// One field as ntsc-rs processes it: a run of plane rows, the field parity
/// they come from, and the frame number apply_effect_to_yiq_field gets.
#[derive(Clone, Copy)]
struct FieldRun {
    first: usize,
    rows: usize,
    parity: YiqField, // Upper / Lower / Both
    frame: usize,
}

/// The fields of a frame (apply_effect_to_all_fields, ntsc.rs lines 1476-1517,
/// and set_from_strided_buffer_maybe_uninit's split, yiq_fielding.rs 581-685).
fn field_runs(field: YiqField, height: usize, frame: usize) -> Vec<FieldRun> {
    match field {
        YiqField::Upper | YiqField::Lower | YiqField::Both => {
            vec![FieldRun { first: 0, rows: field.num_image_rows(height), parity: field, frame }]
        }
        YiqField::InterleavedUpper => {
            let n_upper = YiqField::Upper.num_actual_image_rows(height);
            let mut v = Vec::new();
            if n_upper > 0 {
                v.push(FieldRun { first: 0, rows: n_upper, parity: YiqField::Upper, frame: frame * 2 });
            }
            if height - n_upper > 0 {
                v.push(FieldRun {
                    first: n_upper,
                    rows: height - n_upper,
                    parity: YiqField::Lower,
                    frame: frame * 2 + 1,
                });
            }
            v
        }
        YiqField::InterleavedLower => {
            let n_lower = YiqField::Lower.num_actual_image_rows(height);
            let mut v = Vec::new();
            if n_lower > 0 {
                v.push(FieldRun { first: 0, rows: n_lower, parity: YiqField::Lower, frame: frame * 2 });
            }
            if height - n_lower > 0 {
                v.push(FieldRun {
                    first: n_lower,
                    rows: height - n_lower,
                    parity: YiqField::Upper,
                    frame: frame * 2 + 1,
                });
            }
            v
        }
    }
}

/// Source image row of a field's row (yiq_fielding.rs lines 604-619, full-frame blit).
fn src_row(parity: YiqField, row_idx: usize, height: usize) -> usize {
    if height == 1 {
        return 0;
    }
    match parity {
        YiqField::Upper => row_idx * 2,
        YiqField::Lower => row_idx * 2 + 1,
        _ => row_idx,
    }
}

/// Output row sources of the Bob deinterlace (yiq_fielding.rs lines 745-941,
/// full-frame blit, no flip).
fn out_rows(field: YiqField, height: usize, out: &mut [NtscGpuOutRow]) {
    let num_rows = field.num_image_rows(height);
    let skip_field: usize = match field {
        YiqField::Upper => 1,
        YiqField::Lower => 0,
        _ => 2,
    };
    for (r, o) in out.iter_mut().enumerate().take(height) {
        let (a, b) = match field {
            YiqField::Upper | YiqField::Lower => {
                if (r & 1) == skip_field && r != 0 && r != height - 1 {
                    ((r - 1) >> 1, (r + 1) >> 1)
                } else {
                    let s = (r >> 1).min(num_rows - 1);
                    (s, s)
                }
            }
            YiqField::InterleavedUpper | YiqField::InterleavedLower => {
                let row_offset = match field {
                    YiqField::InterleavedUpper => YiqField::Upper.num_image_rows(height) * (r & 1),
                    _ => YiqField::Lower.num_image_rows(height) * (1 - (r & 1)),
                };
                let s = ((r >> 1) + row_offset).min(height - 1);
                (s, s)
            }
            YiqField::Both => {
                let s = r.min(num_rows - 1);
                (s, s)
            }
        };
        o.a = a as u32;
        o.b = b as u32;
    }
}

const P_Y: u32 = 1;
const P_I: u32 = 2;
const P_Q: u32 = 4;

fn filter_pass(stage: u32, planes: u32, f: [NtscGpuFilter; 3]) -> NtscGpuPass {
    NtscGpuPass { stage: stage as i32, kind: K_FILTER, planes, filter: f, ..Default::default() }
}

fn chroma_lowpass_pass(stage: u32, mode: ChromaLowpass, h: f32, ft: FilterType) -> Option<NtscGpuPass> {
    match mode {
        // composite_chroma_lowpass (ntsc.rs 251-261)
        ChromaLowpass::Full => {
            let i = make_lowpass_for_type(1300000.0, NTSC_RATE * h, ft);
            let q = make_lowpass_for_type(600000.0, NTSC_RATE * h, ft);
            Some(filter_pass(
                stage,
                P_I | P_Q,
                [NtscGpuFilter::default(), i.to_gpu(INIT_ZERO, 2), q.to_gpu(INIT_ZERO, 4)],
            ))
        }
        // composite_chroma_lowpass_lite (ntsc.rs 264-273)
        ChromaLowpass::Light => {
            let f = make_lowpass_for_type(2600000.0, NTSC_RATE * h, ft);
            Some(filter_pass(
                stage,
                P_I | P_Q,
                [NtscGpuFilter::default(), f.to_gpu(INIT_ZERO, 1), f.to_gpu(INIT_ZERO, 1)],
            ))
        }
        ChromaLowpass::None => None,
    }
}

/// A video_noise_line pass (ntsc.rs 607-673): composite_noise / plane_noise
/// with FbmNoiseSettings, the row seeds of `seeds` (ntsc_gpu_row_seeds).
fn noise_pass(stage: u32, planes: u32, seeds: u64, n: &FbmNoiseSettings, h: f32) -> NtscGpuPass {
    let detail: u32 = n.detail.try_into().unwrap_or_default();
    NtscGpuPass {
        stage: stage as i32,
        kind: K_NOISE,
        planes,
        mode: seeds as i32,
        a: n.frequency / h,
        b: n.intensity * 0.25,
        pad: [detail.clamp(1, 5) as i32, 0],
        ..Default::default()
    }
}

/// The GPU pass list, in apply_effect_to_yiq_field order (ntsc.rs 1188-1473).
fn build_passes(e: &NtscEffect, h: f32, v: f32) -> Vec<NtscGpuPass> {
    let mut p = Vec::new();
    let one = |stage: u32, kind: i32| NtscGpuPass { stage: stage as i32, kind, ..Default::default() };
    let on = |stage: u32, kind: i32, planes: u32| NtscGpuPass { stage: stage as i32, kind, planes, ..Default::default() };

    match e.input_luma_filter {
        LumaLowpass::None => {}
        LumaLowpass::Box => p.push(one(ST_LUMA_FILTER, K_LUMA_BOX)),
        LumaLowpass::Notch => p.push(filter_pass(
            ST_LUMA_FILTER,
            P_Y,
            [make_notch_filter(0.5, 2.0).to_gpu(INIT_FIRST_SAMPLE, 0), Default::default(), Default::default()],
        )),
    }
    if let Some(x) = chroma_lowpass_pass(ST_CHROMA_LOWPASS_IN, e.chroma_lowpass_in, h, e.filter_type) {
        p.push(x);
    }
    p.push(one(ST_CHROMA_INTO_LUMA, K_CHROMA_INTO_LUMA));
    if e.composite_sharpening != 0.0 {
        let f = make_lowpass((315000000.0 / 88.0 / 2.0) * h, NTSC_RATE * h).with_scale(-e.composite_sharpening);
        p.push(filter_pass(
            ST_COMPOSITE_SHARPENING,
            P_Y,
            [f.to_gpu(INIT_ZERO, 0), Default::default(), Default::default()],
        ));
    }
    if let Some(n) = &e.composite_noise {
        p.push(noise_pass(ST_COMPOSITE_NOISE, P_Y, noise_seeds::VIDEO_COMPOSITE, n, h));
    }
    if e.snow_intensity > 0.0 && h > 0.0 {
        p.push(on(ST_SNOW, K_SPECKLES, P_Y));
    }
    if e.head_switching.is_some() {
        // head_switching (ntsc.rs 718-796): the shift (and mid-line copy), then the transient
        p.push(on(ST_HEAD_SWITCHING, K_SHIFT_ROWS, P_Y));
        p.push(on(ST_HEAD_SWITCHING, K_SPECKLES, P_Y));
    }
    if e.tracking_noise.is_some() {
        // tracking_noise (ntsc.rs 874-945): per affected row shift, noise line, speckles
        p.push(on(ST_TRACKING_NOISE, K_SHIFT_ROWS, P_Y));
        let mut n = on(ST_TRACKING_NOISE, K_NOISE, P_Y);
        n.mode = SEEDS_TRACKING as i32;
        n.a = 0.25 / h;
        n.b = 0.0; // per row: NtscGpuRowShift::intensity
        n.pad = [1, 1]; // 1 octave; pad[1] = 1: the intensity comes from the row table
        p.push(n);
        p.push(on(ST_TRACKING_NOISE, K_SPECKLES, P_Y));
    }
    {
        let mut d = one(ST_LUMA_INTO_CHROMA, K_DEMOD);
        d.mode = e.chroma_demodulation as i32;
        d.filter[0] = make_notch_filter(0.5, 2.0).to_gpu(INIT_ZERO, 0);
        p.push(d);
    }
    if e.luma_smear > 0.0 {
        // luma_smear (ntsc.rs 577-587): note the rate is the horizontal scale itself.
        let f = make_lowpass(f32::exp2(-4.0 * e.luma_smear) * 0.25, h);
        p.push(filter_pass(ST_LUMA_SMEAR, P_Y, [f.to_gpu(INIT_ZERO, 0), Default::default(), Default::default()]));
    }
    if let Some(r) = &e.ringing {
        let f = make_notch_filter((r.frequency / h).clamp(0.0, 1.0), r.power).with_scale(r.intensity);
        p.push(filter_pass(ST_RINGING, P_Y, [f.to_gpu(INIT_FIRST_SAMPLE, 1), Default::default(), Default::default()]));
    }
    if let Some(n) = &e.luma_noise {
        p.push(noise_pass(ST_LUMA_NOISE, P_Y, noise_seeds::VIDEO_LUMA, n, h));
    }
    if let Some(n) = &e.chroma_noise {
        p.push(noise_pass(ST_CHROMA_NOISE, P_I, noise_seeds::VIDEO_CHROMA_I, n, h));
        p.push(noise_pass(ST_CHROMA_NOISE, P_Q, noise_seeds::VIDEO_CHROMA_Q, n, h));
    }
    if e.chroma_phase_error > 0.0 {
        // chroma_phase_error -> chroma_phase_offset_line (ntsc.rs 676-699)
        let phase_shift = e.chroma_phase_error * PI * 2.0;
        let (s, c) = phase_shift.sin_cos();
        let mut x = one(ST_CHROMA_PHASE_ERROR, K_ROTATE_CONST);
        x.a = s;
        x.b = c;
        p.push(x);
    }
    if e.chroma_phase_noise_intensity > 0.0 {
        p.push(one(ST_CHROMA_PHASE_NOISE, K_ROTATE_ROWS));
    }
    if e.chroma_delay_horizontal != 0.0 || e.chroma_delay_vertical != 0 {
        // chroma_delay (ntsc.rs 969-1051)
        let off0 = e.chroma_delay_horizontal * h;
        let off1 = ((e.chroma_delay_vertical as isize as f32) * v).round() as isize;
        let mut x = on(ST_CHROMA_DELAY, K_CHROMA_DELAY, P_I | P_Q);
        x.a = off0;
        x.mode = off1 as i32;
        x.pad[0] = (off0.abs() == 0.0) as i32;
        p.push(x);
    }
    if let Some(v) = &e.vhs_settings {
        if v.edge_wave.as_ref().is_some_and(|w| w.intensity > 0.0) {
            // vhs_edge_wave (ntsc.rs 1054-1085): one shift per row, all three planes
            p.push(on(ST_EDGE_WAVE, K_SHIFT_ROWS, P_Y | P_I | P_Q));
        }
        if let Some(t) = tape_params(v.tape_speed) {
            // ntsc.rs 1348-1407
            let chroma_delay = (t.chroma_delay as f32 * h).round() as usize;
            let lf = make_lowpass_for_type(t.luma_cut, NTSC_RATE * h, e.filter_type);
            let cf = make_lowpass_for_type(t.chroma_cut, NTSC_RATE * h, e.filter_type);
            p.push(filter_pass(
                ST_TAPE_SPEED,
                P_Y | P_I | P_Q,
                [lf.to_gpu(INIT_ZERO, 0), cf.to_gpu(INIT_ZERO, chroma_delay), cf.to_gpu(INIT_ZERO, chroma_delay)],
            ));
            let single = make_lowpass(t.luma_cut, NTSC_RATE * h).with_scale(-1.6);
            p.push(filter_pass(
                ST_TAPE_SPEED,
                P_Y,
                [single.to_gpu(INIT_ZERO, 0), Default::default(), Default::default()],
            ));
        }
        if v.chroma_loss > 0.0 {
            p.push(one(ST_CHROMA_LOSS, K_CHROMA_LOSS));
        }
        if let (Some(sharpen), Some(t)) = (&v.sharpen, tape_params(v.tape_speed)) {
            // ntsc.rs 1413-1441
            let mult = match e.filter_type {
                FilterType::ConstantK => 4.0,
                FilterType::Butterworth => 1.0,
            };
            let f = make_lowpass_for_type(t.luma_cut * mult * sharpen.frequency, NTSC_RATE * h, e.filter_type)
                .with_scale(-sharpen.intensity * 2.0 * sharpen.frequency);
            p.push(filter_pass(ST_SHARPEN, P_Y, [f.to_gpu(INIT_ZERO, 0), Default::default(), Default::default()]));
        }
    }
    if e.chroma_vert_blend {
        p.push(one(ST_VERT_BLEND, K_VERT_BLEND));
    }
    if let Some(x) = chroma_lowpass_pass(ST_CHROMA_LOWPASS_OUT, e.chroma_lowpass_out, h, e.filter_type) {
        p.push(x);
    }
    p
}

/// ntsc-rs's NtscEffect for this frame: the instance's settings as
/// process_impl prepares them, with `stage_mask` applied.
fn masked_effect(inst: &NtscInstance, scale_factor: [f32; 2], seed_offset: i32, stage_mask: u32) -> NtscEffect {
    let mut e = effect_for(inst, scale_factor, seed_offset);
    mask_effect(&mut e, stage_mask);
    e
}

/// Resolves the GPU plan of one frame. See ntscrs_capi.h.
///
/// # Safety
/// `inst` valid; `info` non-null; `passes` has `max_passes` entries; `rows`
/// and `out` have `height` entries (or are null).
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn ntsc_gpu_plan(
    inst: *const NtscInstance,
    width: u32,
    height: u32,
    frame_index: i64,
    scale_x: f32,
    scale_y: f32,
    seed_offset: i32,
    stage_mask: u32,
    info: *mut NtscGpuPlanInfo,
    passes: *mut NtscGpuPass,
    max_passes: u32,
    rows: *mut NtscGpuRow,
    out: *mut NtscGpuOutRow,
) -> i32 {
    let Some(inst) = inst.as_ref() else { return 1 };
    let Some(info) = info.as_mut() else { return 1 };
    if width == 0 || height == 0 || !(scale_x.is_finite() && scale_y.is_finite()) || scale_x <= 0.0 || scale_y <= 0.0 {
        return 1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let (w, hgt) = (width as usize, height as usize);
        let frame = frame_index.max(0) as usize;
        let e = masked_effect(inst, [scale_x, scale_y], seed_offset, stage_mask);
        let (seed, hs, vs) = common_info(&e, hgt, [scale_x, scale_y]);
        let field = e.use_field.to_yiq_field(frame);
        let runs = field_runs(field, hgt, frame);

        let enabled = enabled_stages(&e, hs);
        let mut unsupported = enabled & !GPU_IMPLEMENTED;

        // chroma_vert_blend takes 2 rows of a field's scratch (ntsc.rs 1123), which a
        // field of one row doesn't have: ntsc-rs panics (the frame fails). Leave such
        // frames to it, so both paths fail alike.
        if e.chroma_vert_blend && hgt >= 2 && runs.iter().any(|r| r.rows < 2) {
            unsupported |= 1 << ST_VERT_BLEND;
        }

        let mut list = build_passes(&e, hs, vs);
        // The comb filters fall back to the notch on a 1-row field (luma_into_chroma,
        // ntsc.rs 488-495). Every field 1 row: the notch for all; some (an
        // interleaved picture 3 rows tall): pad[0] = 1, the notch on the rows of
        // 1-row fields and the comb on the others.
        let ones = runs.iter().filter(|r| field_num_rows(r.parity, hgt) == 1).count();
        for p in list.iter_mut() {
            if p.kind == K_DEMOD && (p.mode == 2 || p.mode == 3) && ones != 0 {
                if ones == runs.len() {
                    p.mode = ChromaDemodulationFilter::Notch as i32;
                } else {
                    p.pad[0] = 1;
                }
            }
        }

        let plane_rows = field.num_image_rows(hgt);
        *info = NtscGpuPlanInfo {
            field: field_code(field),
            plane_rows: plane_rows as u32,
            num_fields: runs.len() as u32,
            num_passes: list.len() as u32,
            unsupported,
            enabled,
            horizontal_scale: hs,
            vertical_scale: vs,
            seed,
            ..Default::default()
        };
        for (k, r) in runs.iter().enumerate().take(2) {
            info.field_first[k] = r.first as u32;
            info.field_rows[k] = r.rows as u32;
            info.field_frame[k] = r.frame as u64;
        }
        if !passes.is_null() {
            let dst = std::slice::from_raw_parts_mut(passes, max_passes as usize);
            for (d, s) in dst.iter_mut().zip(list.iter()) {
                *d = *s;
            }
        }
        if !rows.is_null() {
            let dst = std::slice::from_raw_parts_mut(rows, hgt);
            fill_rows(&e, &runs, hgt, seed, dst);
        }
        if !out.is_null() {
            let dst = std::slice::from_raw_parts_mut(out, hgt);
            out_rows(field, hgt, dst);
        }
        let _ = w;
        if list.len() > max_passes as usize {
            3
        } else {
            0
        }
    }));
    result.unwrap_or(2)
}

/// `yiq.num_rows()` of a field's view (yiq_fielding.rs 489-491).
fn field_num_rows(parity: YiqField, height: usize) -> usize {
    parity.num_image_rows(height)
}

fn fill_rows(e: &NtscEffect, runs: &[FieldRun], height: usize, seed: u64, dst: &mut [NtscGpuRow]) {
    for r in runs {
        // chroma_phase_noise (ntsc.rs 702-715)
        let phase_seeder =
            Seeder::new(seed).mix(noise_seeds::VIDEO_CHROMA_PHASE).mix(r.frame);
        for k in 0..r.rows {
            let row = &mut dst[r.first + k];
            row.src_row = src_row(r.parity, k, height) as u32;
            row.field_row = k as u32;
            row.field_first = r.first as u32;
            row.field_rows = r.rows as u32;
            row.xi = chroma_phase_shift(
                e.video_scanline_phase_shift,
                e.video_scanline_phase_shift_offset,
                r.frame,
                k * 2,
            ) as u32;
            row.flags = 0;
            if e.chroma_phase_noise_intensity > 0.0 {
                let phase_shift = (phase_seeder.clone().mix(k).finalize_f32() - 0.5)
                    * 2.0
                    * e.chroma_phase_noise_intensity;
                let (s, c) = (phase_shift * PI * 2.0).sin_cos();
                row.phase_sin = s;
                row.phase_cos = c;
            } else {
                row.phase_sin = 0.0;
                row.phase_cos = 1.0;
            }
        }
        // chroma_loss (ntsc.rs 1088-1115): rows of this field's view
        if let Some(v) = &e.vhs_settings {
            if v.chroma_loss > 0.0 {
                let s = Seeder::new(seed).mix(noise_seeds::CHROMA_LOSS).mix(r.frame).finalize_u64();
                let mut rng = Xoshiro256PlusPlus::seed_from_u64(s);
                let dist = Geometric::new(v.chroma_loss as f64);
                let view_rows = field_num_rows(r.parity, height);
                let mut row_idx = 0usize;
                loop {
                    row_idx += rng.sample(&dist);
                    if row_idx >= view_rows {
                        break;
                    }
                    if row_idx < r.rows {
                        dst[r.first + row_idx].flags |= ROW_FLAG_CHROMA_LOSS;
                    }
                    row_idx += 1;
                }
            }
        }
    }
}

/// The frame as the table exports see it: the effect (as process_impl builds
/// it), CommonInfo's seed / scales, and the fields.
struct FrameSetup {
    e: NtscEffect,
    seed: u64,
    hs: f32,
    vs: f32,
    runs: Vec<FieldRun>,
    width: usize,
}

fn frame_setup(inst: &NtscInstance, width: u32, height: u32, frame_index: i64, sx: f32, sy: f32, seed_offset: i32) -> FrameSetup {
    let hgt = height as usize;
    let frame = frame_index.max(0) as usize;
    let e = effect_for(inst, [sx, sy], seed_offset);
    let (seed, hs, vs) = common_info(&e, hgt, [sx, sy]);
    let field = e.use_field.to_yiq_field(frame);
    let runs = field_runs(field, hgt, frame);
    FrameSetup { e, seed, hs, vs, runs, width: width as usize }
}

/// The tables' thread pool: rows are independent, as in ntsc-rs (which runs
/// them on its own rayon pool, thread_pool.rs; same size: physical cores).
fn pool() -> &'static rayon_core::ThreadPool {
    static POOL: std::sync::OnceLock<rayon_core::ThreadPool> = std::sync::OnceLock::new();
    POOL.get_or_init(|| {
        let mut b = rayon_core::ThreadPoolBuilder::new().stack_size(2 * 1024 * 1024);
        if std::env::var("RAYON_NUM_THREADS").is_err() {
            b = b.num_threads(num_cpus::get_physical());
        }
        b.build().unwrap()
    })
}

/// `f` over 0..n in up to 64 parallel chunks (in order: the pool's threads
/// take them as they finish, so uneven rows balance out); one chunk when n is small.
fn par_chunks<T: Send + Default>(n: usize, f: impl Fn(std::ops::Range<usize>) -> T + Sync) -> Vec<T> {
    if n < 16 {
        return vec![f(0..n)];
    }
    let chunks = n.min(64);
    let mut out: Vec<T> = (0..chunks).map(|_| T::default()).collect();
    pool().scope(|s| {
        for (c, slot) in out.iter_mut().enumerate() {
            let f = &f;
            let range = (n * c / chunks)..(n * (c + 1) / chunks);
            s.spawn(move |_| *slot = f(range));
        }
    });
    out
}

/// One row of a field for the table loops: plane row, field (index into runs), row index ntsc-rs uses.
#[derive(Clone, Copy)]
struct RowItem {
    plane_row: usize,
    run: usize,
    index: usize,
}

/// The rows tracking_noise touches in a field of `height` rows (ntsc.rs 883-897):
/// (num_rows, start_row, cut_off_rows).
fn tracking_rows(height_setting: i32, vs: f32, height: usize) -> (usize, usize, usize) {
    let num_rows: usize = height_setting.try_into().unwrap_or_default();
    let num_rows = (num_rows as f32 * vs).round() as usize;
    let start_row = height.max(num_rows) - num_rows;
    let cut_off_rows = num_rows.saturating_sub(height);
    (num_rows, start_row, cut_off_rows)
}

/// Per-row noise seeds of one noise pass (video_noise_line, ntsc.rs 607-630),
/// for every plane row. `pass` is one of the `noise_seeds` constants used with
/// plane_noise / composite_noise (0 composite, 1 chroma I, 9 chroma Q, 10 luma),
/// or SEEDS_TRACKING (3): tracking noise's noise lines (ntsc.rs 887-935), only
/// its affected rows (the others are zero).
///
/// # Safety
/// `inst` valid; `out` has `height` entries.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn ntsc_gpu_row_seeds(
    inst: *const NtscInstance,
    width: u32,
    height: u32,
    frame_index: i64,
    scale_x: f32,
    scale_y: f32,
    seed_offset: i32,
    pass: u32,
    out: *mut NtscGpuRowSeed,
) -> i32 {
    let Some(inst) = inst.as_ref() else { return 1 };
    if out.is_null() || width == 0 || height == 0 {
        return 1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let f = frame_setup(inst, width, height, frame_index, scale_x, scale_y, seed_offset);
        let dst = std::slice::from_raw_parts_mut(out, height as usize);
        // video_noise_line's first draws (ntsc.rs 617-619), with its `index as u64`.
        let row_seed = |seeder: &Seeder, index: usize| {
            let s = seeder.clone().mix(index as u64).finalize_u64();
            let mut rng = Xoshiro256PlusPlus::seed_from_u64(s);
            let noise_seed = rng.next_u32();
            let offset = rng.random::<f32>() * width as f32;
            NtscGpuRowSeed { seed_lo: s as u32, seed_hi: (s >> 32) as u32, noise_seed: noise_seed as i32, offset }
        };
        let mut items = Vec::new();
        let seeders: Vec<Seeder> = if pass == SEEDS_TRACKING {
            dst.fill(NtscGpuRowSeed::default());
            let Some(t) = &f.e.tracking_noise else { return 0 };
            for (ri, r) in f.runs.iter().enumerate() {
                let (_, start_row, cut_off_rows) = tracking_rows(t.height, f.vs, r.rows);
                for i in 0..r.rows - start_row {
                    items.push(RowItem { plane_row: r.first + start_row + i, run: ri, index: i + cut_off_rows });
                }
            }
            f.runs.iter().map(|r| Seeder::new(f.seed).mix(noise_seeds::TRACKING_NOISE).mix(r.frame).mix(2)).collect()
        } else {
            for (ri, r) in f.runs.iter().enumerate() {
                for k in 0..r.rows {
                    items.push(RowItem { plane_row: r.first + k, run: ri, index: k });
                }
            }
            f.runs.iter().map(|r| Seeder::new(f.seed).mix(pass as u64).mix(r.frame)).collect()
        };
        // Serial: ~30 ns a row, less than waking the pool.
        for it in &items {
            dst[it.plane_row] = row_seed(&seeders[it.run], it.index);
        }
        0
    }));
    result.unwrap_or(2)
}

/// Per-row shifts of head switching (ntsc.rs 718-796), tracking noise (874-926)
/// or the edge wave (1054-1085), for every plane row (`boundary` SHIFT_NONE:
/// the row is left alone). The noise behind the tracking and edge-wave shifts
/// is sampled with an exact copy of ntsc-rs's code (gpu_noise.rs), the
/// head-switching powf and seeds as ntsc-rs computes them.
///
/// # Safety
/// `inst` valid; `out` has `height` entries.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn ntsc_gpu_row_shifts(
    inst: *const NtscInstance,
    width: u32,
    height: u32,
    frame_index: i64,
    scale_x: f32,
    scale_y: f32,
    seed_offset: i32,
    stage: u32,
    out: *mut NtscGpuRowShift,
) -> i32 {
    let Some(inst) = inst.as_ref() else { return 1 };
    if out.is_null() || width == 0 || height == 0 {
        return 1;
    }
    if !matches!(stage, ST_HEAD_SWITCHING | ST_TRACKING_NOISE | ST_EDGE_WAVE) {
        return 1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let f = frame_setup(inst, width, height, frame_index, scale_x, scale_y, seed_offset);
        let dst = std::slice::from_raw_parts_mut(out, height as usize);
        dst.fill(NtscGpuRowShift { shift: 0.0, boundary: SHIFT_NONE, copy_start: -1, intensity: 0.0 });
        match stage {
            ST_HEAD_SWITCHING => head_switching_rows(&f, |row, rs| dst[row] = rs, |_, _, _| {}),
            ST_TRACKING_NOISE => {
                let Some(t) = &f.e.tracking_noise else { return 0 };
                for r in &f.runs {
                    let (num_rows, start_row, cut_off_rows) = tracking_rows(t.height, f.vs, r.rows);
                    let height = r.rows;
                    let seeder = Seeder::new(f.seed).mix(noise_seeds::TRACKING_NOISE).mix(r.frame);
                    let noise_seed = seeder.clone().mix(0).finalize_i32();
                    let offset = seeder.clone().mix(1).finalize_f32() * height as f32;
                    let n = num_rows.min(height);
                    let mut shift_noise = vec![0f32; n];
                    let noise = Simplex { seed: noise_seed, frequency: 0.5 };
                    sample_noise_1d::<Simplex1d, _>(Level::new(), &noise, [offset], [n], &mut shift_noise);
                    for i in 0..height - start_row {
                        let index = i + cut_off_rows;
                        let intensity_scale = index as f32 / num_rows as f32;
                        dst[r.first + start_row + i] = NtscGpuRowShift {
                            shift: shift_noise[index - cut_off_rows]
                                * intensity_scale
                                * t.wave_intensity
                                * 0.25
                                * f.hs,
                            boundary: 0,
                            copy_start: -1,
                            intensity: (intensity_scale.powi(2) * t.noise_intensity * 4.0) * 0.25,
                        };
                    }
                }
            }
            _ => {
                let Some(ew) = f.e.vhs_settings.as_ref().and_then(|v| v.edge_wave.as_ref()) else { return 0 };
                if !(ew.intensity > 0.0) {
                    return 0;
                }
                for r in &f.runs {
                    let height = r.rows;
                    let seeder = Seeder::new(f.seed).mix(noise_seeds::EDGE_WAVE);
                    let noise_seed: i32 = seeder.clone().mix(0).finalize_i32();
                    let offset = seeder.mix(1).finalize_f32() * height as f32;
                    let mut noise_dest = vec![0f32; height];
                    let noise = Fbm {
                        seed: noise_seed,
                        octaves: ew.detail.clamp(1, 5) as usize,
                        gain: std::f32::consts::FRAC_1_SQRT_2,
                        lacunarity: 2.0,
                        frequency: ew.frequency / f.vs,
                    };
                    sample_noise_2d::<Simplex2d, _>(
                        Level::new(),
                        &noise,
                        [offset, r.frame as f32 * ew.speed],
                        [height, 1],
                        &mut noise_dest,
                    );
                    for index in 0..height {
                        dst[r.first + index] = NtscGpuRowShift {
                            shift: (noise_dest[index] / 0.022) * ew.intensity * 0.5 * f.hs,
                            boundary: 1,
                            copy_start: -1,
                            intensity: 0.0,
                        };
                    }
                }
            }
        }
        0
    }));
    result.unwrap_or(2)
}

/// head_switching's rows (ntsc.rs 718-796): `row(plane_row, shift)` for each
/// affected row, `transient(plane_row, copy_start, values)` for the mid line.
fn head_switching_rows(
    f: &FrameSetup,
    mut row: impl FnMut(usize, NtscGpuRowShift),
    mut transient: impl FnMut(usize, usize, &[f32]),
) {
    let Some(hsw) = &f.e.head_switching else { return };
    let num_rows: usize = hsw.height.try_into().unwrap_or_default();
    let offset: usize = hsw.offset.try_into().unwrap_or_default();
    let shift = hsw.horiz_shift;
    let num_rows = (num_rows as f32 * f.vs).round() as usize;
    let offset = (offset as f32 * f.vs).round() as usize;
    if offset > num_rows {
        return;
    }
    let num_affected_rows = num_rows - offset;
    let width = f.width;
    for r in &f.runs {
        let height = r.rows;
        let start_row = height.max(num_affected_rows) - num_affected_rows;
        let cut_off_rows = num_affected_rows.saturating_sub(height);
        let seeder = Seeder::new(f.seed).mix(noise_seeds::HEAD_SWITCHING).mix(r.frame);
        for i in 0..height - start_row {
            let index = num_affected_rows - (i + cut_off_rows);
            let row_shift = shift * ((index + offset) as f32 / num_rows as f32).powf(1.5);
            let noisy_shift = (row_shift + (seeder.clone().mix(index).finalize_f32() - 0.5)) * f.hs;
            let plane_row = r.first + start_row + i;
            let mut rs = NtscGpuRowShift { shift: noisy_shift, boundary: 0, copy_start: -1, intensity: 0.0 };
            if index == num_affected_rows {
                if let Some(mid_line) = &hsw.mid_line {
                    let seeder =
                        Seeder::new(f.seed).mix(noise_seeds::HEAD_SWITCHING_MID_LINE_JITTER).mix(r.frame);
                    let jitter_rand =
                        (seeder.clone().mix(0).finalize_f32() + seeder.clone().mix(1).finalize_f32()) * 0.5;
                    let jitter = (jitter_rand - 0.5) * mid_line.jitter;
                    let copy_start = (width as f32 * (mid_line.position + jitter)) as usize;
                    if copy_start > width {
                        // ntsc-rs returns before copying: the row stays as it was
                        rs.boundary = SHIFT_NONE;
                    } else {
                        let transient_intensity = (seeder.clone().mix(0).finalize_f32() + 0.5) * 0.5;
                        let transient_len = 16.0 * f.hs;
                        let mut values = Vec::new();
                        for i in copy_start..(copy_start + transient_len.ceil() as usize).min(width) {
                            let x = (i - copy_start) as f32;
                            values.push((1.0 - (x / transient_len)).powi(3) * transient_intensity);
                        }
                        rs.copy_start = copy_start as i32;
                        rs.intensity = transient_intensity;
                        transient(plane_row, copy_start, &values);
                    }
                }
            }
            row(plane_row, rs);
        }
    }
}

/// Speckle events being built: (row, first pixel, values).
#[derive(Default)]
struct Speckles {
    events: Vec<NtscGpuSpeckle>,
    values: Vec<f32>,
}
impl Speckles {
    fn push(&mut self, row: usize, x0: usize, values: &[f32]) {
        if values.is_empty() {
            return;
        }
        self.events.push(NtscGpuSpeckle {
            row: row as u32,
            x0: x0 as i32,
            count: values.len() as u32,
            value_offset: self.values.len() as u32,
        });
        self.values.extend_from_slice(values);
    }
}

/// Copy of ntsc-rs's row_speckles (ntsc.rs 799-869), with `row[i] += value`
/// replaced by recording the value (the GPU adds them in the same order).
fn row_speckles(
    out: &mut Speckles,
    plane_row: usize,
    row_len: usize,
    rng: &mut Xoshiro256PlusPlus,
    intensity: f32,
    anisotropy: f32,
    horizontal_scale: f32,
) {
    let intensity = intensity as f64;
    let anisotropy = anisotropy as f64;
    const TRANSIENT_LEN_RANGE: std::ops::RangeInclusive<f32> = 8.0..=64.0;

    let logistic_factor =
        ((rng.random::<f64>() - intensity) / (intensity * (1.0 - intensity) * (1.0 - anisotropy))).exp();
    let mut line_snow_intensity = anisotropy / (1.0 + logistic_factor) + intensity * (1.0 - anisotropy);

    line_snow_intensity *= 0.125;
    line_snow_intensity = line_snow_intensity.clamp(0.0, 1.0);
    if line_snow_intensity <= 0.0 {
        return;
    }

    let dist = Geometric::new(line_snow_intensity);
    let mut pixel_idx = (-TRANSIENT_LEN_RANGE.end()).floor() as isize;
    let mut values: Vec<f32> = Vec::new();
    loop {
        pixel_idx += rng.sample(&dist).min(isize::MAX as usize) as isize;
        if pixel_idx >= row_len as isize {
            break;
        }

        let transient_len: f32 = rng.random_range(TRANSIENT_LEN_RANGE) * horizontal_scale;
        let transient_freq = rng.random_range(transient_len * 3.0..=transient_len * 5.0);
        let pixel_idx_end = pixel_idx + transient_len.ceil() as isize;

        rng.jump();
        let mut transient_rng = rng.clone();

        values.clear();
        let start = pixel_idx.clamp(0, row_len as isize);
        for i in start..pixel_idx_end.clamp(0, row_len as isize) {
            let x = (i - pixel_idx) as f32;
            values.push(
                ((x * PI) / transient_freq).cos()
                    * (1.0 - x / transient_len).powi(2)
                    * transient_rng.random_range(-1.0..2.0),
            );
        }
        out.push(plane_row, start as usize, &values);

        pixel_idx += 1;
    }
}

/// Speckle events (additive values per pixel run) of snow (ntsc.rs 948-964),
/// tracking noise (937-943) or the head-switching transient (779-786).
///
/// # Safety
/// `inst` valid; `events` max_events entries, `values` max_values entries (or
/// null with max 0); `num_events`, `num_values` non-null.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn ntsc_gpu_speckles(
    inst: *const NtscInstance,
    width: u32,
    height: u32,
    frame_index: i64,
    scale_x: f32,
    scale_y: f32,
    seed_offset: i32,
    stage: u32,
    events: *mut NtscGpuSpeckle,
    max_events: u32,
    values: *mut f32,
    max_values: u32,
    num_events: *mut u32,
    num_values: *mut u32,
) -> i32 {
    let Some(inst) = inst.as_ref() else { return 1 };
    if num_events.is_null() || num_values.is_null() || width == 0 || height == 0 {
        return 1;
    }
    if !matches!(stage, ST_SNOW | ST_TRACKING_NOISE | ST_HEAD_SWITCHING) {
        return 1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let f = frame_setup(inst, width, height, frame_index, scale_x, scale_y, seed_offset);
        let mut sp = Speckles::default();
        let w = f.width;
        // Rows in parallel chunks, each its own list, joined in row order.
        fn join(sp: &mut Speckles, parts: Vec<Speckles>) {
            for part in parts {
                let base = sp.values.len() as u32;
                sp.events.extend(part.events.iter().map(|e| NtscGpuSpeckle { value_offset: e.value_offset + base, ..*e }));
                sp.values.extend_from_slice(&part.values);
            }
        }
        match stage {
            ST_SNOW => {
                if f.e.snow_intensity > 0.0 && f.hs > 0.0 {
                    let intensity = f.e.snow_intensity * 0.01;
                    let seeders: Vec<Seeder> =
                        f.runs.iter().map(|r| Seeder::new(f.seed).mix(noise_seeds::SNOW).mix(r.frame)).collect();
                    let mut items = Vec::new();
                    for (ri, r) in f.runs.iter().enumerate() {
                        for index in 0..r.rows {
                            items.push(RowItem { plane_row: r.first + index, run: ri, index });
                        }
                    }
                    join(&mut sp, par_chunks(items.len(), |range| {
                        let mut part = Speckles::default();
                        for it in &items[range] {
                            let line_seed = seeders[it.run].clone().mix(it.index);
                            let mut rng = Xoshiro256PlusPlus::seed_from_u64(line_seed.finalize_u64());
                            row_speckles(&mut part, it.plane_row, w, &mut rng, intensity, f.e.snow_anisotropy, f.hs);
                        }
                        part
                    }));
                }
            }
            ST_TRACKING_NOISE => {
                if let Some(t) = &f.e.tracking_noise {
                    let seeders: Vec<Seeder> = f
                        .runs
                        .iter()
                        .map(|r| Seeder::new(f.seed).mix(noise_seeds::TRACKING_NOISE).mix(r.frame).mix(2))
                        .collect();
                    let mut items = Vec::new();
                    let mut num_rows = 0;
                    for (ri, r) in f.runs.iter().enumerate() {
                        let (n, start_row, cut_off_rows) = tracking_rows(t.height, f.vs, r.rows);
                        num_rows = n;
                        for i in 0..r.rows - start_row {
                            items.push(RowItem { plane_row: r.first + start_row + i, run: ri, index: i + cut_off_rows });
                        }
                    }
                    join(&mut sp, par_chunks(items.len(), |range| {
                        let mut part = Speckles::default();
                        for it in &items[range] {
                            let intensity_scale = it.index as f32 / num_rows as f32;
                            row_speckles(
                                &mut part,
                                it.plane_row,
                                w,
                                &mut Xoshiro256PlusPlus::seed_from_u64(seeders[it.run].clone().mix(it.index).finalize_u64()),
                                t.snow_intensity * intensity_scale.powi(2),
                                t.snow_anisotropy,
                                f.hs,
                            );
                        }
                        part
                    }));
                }
            }
            _ => head_switching_rows(&f, |_, _| {}, |row, x0, v| sp.push(row, x0, v)),
        }
        *num_events = sp.events.len() as u32;
        *num_values = sp.values.len() as u32;
        if sp.events.len() > max_events as usize || sp.values.len() > max_values as usize {
            return 3;
        }
        if !sp.events.is_empty() {
            std::slice::from_raw_parts_mut(events, sp.events.len()).copy_from_slice(&sp.events);
        }
        if !sp.values.is_empty() {
            std::slice::from_raw_parts_mut(values, sp.values.len()).copy_from_slice(&sp.values);
        }
        0
    }));
    result.unwrap_or(2)
}

/// `S::f32s::N` of ntsc-rs's SIMD level on this CPU (gpu_noise::simd_lanes).
#[no_mangle]
pub extern "C" fn ntsc_gpu_simd_lanes() -> u32 {
    crate::gpu_noise::simd_lanes() as u32
}

/// Debug / parity: runs ntsc-rs itself on a frame with only the stages in
/// `stage_mask` (the two modulation stages always run) and returns the YIQ
/// planes before deinterlacing: `planes` gets Y, then I, then Q, each
/// width * plane_rows floats (plane_rows = NtscGpuPlanInfo::plane_rows).
/// `planes_len` is its length in floats.
///
/// # Safety
/// `inst` valid; `data` row_bytes * height bytes; `planes` planes_len floats.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn ntsc_debug_process_yiq(
    inst: *const NtscInstance,
    fmt: i32,
    data: *const u8,
    width: u32,
    height: u32,
    row_bytes: u32,
    frame_index: i64,
    scale_x: f32,
    scale_y: f32,
    seed_offset: i32,
    stage_mask: u32,
    planes: *mut f32,
    planes_len: usize,
) -> i32 {
    let Some(inst) = inst.as_ref() else { return 1 };
    if data.is_null() || planes.is_null() || width == 0 || height == 0 || (row_bytes as u64) < (width as u64) * 4 {
        return 1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let buf = std::slice::from_raw_parts(data, row_bytes as usize * height as usize);
        let dims = (width as usize, height as usize);
        let frame = frame_index.max(0) as usize;
        let effect = masked_effect(inst, [scale_x, scale_y], seed_offset, stage_mask);
        let field = effect.use_field.to_yiq_field(frame);
        let n = dims.0 * field.num_image_rows(dims.1);
        if planes_len < n * 3 {
            return 3;
        }
        let mut yiq_buf = vec![0f32; YiqView::buf_length_for(dims, field)];
        let mut view = YiqView::from_parts(&mut yiq_buf, dims, field);
        let blit = BlitInfo::from_full_frame(dims.0, dims.1, row_bytes as usize);
        match fmt {
            NTSC_BGRA8 => view.set_from_strided_buffer::<Bgrx, u8, _>(buf, blit, ()),
            NTSC_RGBA8 => view.set_from_strided_buffer::<Rgbx, u8, _>(buf, blit, ()),
            _ => return 1,
        }
        effect.apply_effect_to_yiq(&mut view, frame, [scale_x, scale_y]);
        let dst = std::slice::from_raw_parts_mut(planes, n * 3);
        dst[..n].copy_from_slice(&view.y[..n]);
        dst[n..2 * n].copy_from_slice(&view.i[..n]);
        dst[2 * n..].copy_from_slice(&view.q[..n]);
        0
    }));
    result.unwrap_or(2)
}

/// The stages the GPU implements (bit per stage), for reports.
#[no_mangle]
pub extern "C" fn ntsc_gpu_implemented_stages() -> u32 {
    GPU_IMPLEMENTED
}
