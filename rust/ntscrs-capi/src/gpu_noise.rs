//! Exact copy of ntsc-rs's gradient noise (vendor/ntsc-rs/crates/ntscrs/src/
//! noise.rs lines 1-82, 115-150, 163-171, 173-235; noise/simplex.rs; noise/
//! grid.rs; noise/hash.rs), for the GPU plan's CPU-side tables (gpu_plan.rs):
//! tracking noise's per-row shift noise (`sample_noise_1d`, `Simplex`) and the
//! edge wave's per-row shifts (`sample_noise_2d`, `Fbm`). ntsc-rs keeps
//! `mod noise` private, so it can't be called from here.
//!
//! Same code, same crate (fearless_simd at ntsc-rs's version, Cargo.lock),
//! same SIMD level (`Level::new()`, as apply_effect_to_yiq_field), so the
//! results are bit-identical to what ntsc-rs computes. The per-pixel noise
//! (composite, luma, chroma, tracking-noise lines) runs on the GPU instead
//! (shaders/ntsc_gpu.slang `fbm_1d`), which follows the same operations.
//!
//! The source is "Adapted from Clatter (<https://github.com/Ralith/clatter/>)
//! which is available under MIT/Apache-2.0/zlib", as ntsc-rs's files say.

use fearless_simd::{dispatch, prelude::*, Level, Simd, SimdFloat};

pub trait Noise<BaseNoise: Sampleable> {
    fn generate<S: Simd>(&self, point: <BaseNoise::Coords as ScalarCoords>::SimdCoords<S>) -> S::f32s;
}

pub struct Fbm {
    pub seed: i32,
    pub octaves: usize,
    pub gain: f32,
    pub lacunarity: f32,
    pub frequency: f32,
}

impl<BaseNoise: Sampleable> Noise<BaseNoise> for Fbm {
    #[inline(always)]
    fn generate<S: Simd>(&self, mut point: <BaseNoise::Coords as ScalarCoords>::SimdCoords<S>) -> <S as Simd>::f32s {
        point = BaseNoise::Coords::map(point, |x| x * self.frequency);
        let mut result = BaseNoise::sample(self.seed, point);

        let mut amplitude = self.gain;
        for _ in 1..self.octaves {
            point = BaseNoise::Coords::map(point, |x| x * self.lacunarity);
            result = BaseNoise::sample(self.seed, point).mul_add(amplitude, result);
            amplitude *= self.gain;
        }
        result
    }
}

pub struct Simplex {
    pub seed: i32,
    pub frequency: f32,
}
impl<BaseNoise: Sampleable> Noise<BaseNoise> for Simplex {
    #[inline(always)]
    fn generate<S: Simd>(&self, point: <BaseNoise::Coords as ScalarCoords>::SimdCoords<S>) -> <S as Simd>::f32s {
        BaseNoise::sample(self.seed, BaseNoise::Coords::map(point, |x| x * self.frequency))
    }
}

#[inline(always)]
fn sample_noise_1d_inner<S: Simd, B: Sampleable<Coords = [f32; 1]>, N: Noise<B>>(
    simd: S,
    noise: &N,
    start_coords: [f32; 1],
    dimensions: [usize; 1],
    destination: &mut [f32],
) {
    assert_eq!(destination.len(), dimensions[0]);

    let mut chunks = destination.chunks_exact_mut(S::f32s::N);
    let mut coords = start_coords.splat_x(simd);
    for chunk in chunks.by_ref() {
        let noise = noise.generate::<S>(coords);
        chunk.copy_from_slice(noise.as_slice());
        coords[0] += S::f32s::N as f32;
    }
    let remainder = chunks.into_remainder();
    if !remainder.is_empty() {
        let noise = noise.generate::<S>(coords);
        remainder.copy_from_slice(&noise.as_slice()[..remainder.len()]);
    }
}

#[inline(always)]
fn sample_noise_2d_inner<S: Simd, B: Sampleable<Coords = [f32; 2]>, N: Noise<B>>(
    simd: S,
    noise: &N,
    mut start_coords: [f32; 2],
    dimensions: [usize; 2],
    destination: &mut [f32],
) {
    assert_eq!(destination.len(), dimensions[0] * dimensions[1]);

    for row in destination.chunks_exact_mut(dimensions[0]) {
        let mut chunks = row.chunks_exact_mut(S::f32s::N);
        let mut coords = start_coords.splat_x(simd);
        for chunk in chunks.by_ref() {
            let noise = noise.generate::<S>(coords);
            chunk.copy_from_slice(noise.as_slice());
            coords[0] += S::f32s::N as f32;
        }
        let remainder = chunks.into_remainder();
        if !remainder.is_empty() {
            let noise = noise.generate::<S>(coords);
            remainder.copy_from_slice(&noise.as_slice()[..remainder.len()]);
        }
        start_coords[1] += 1.0;
    }
}

pub fn sample_noise_1d<B: Sampleable<Coords = [f32; 1]>, N: Noise<B>>(
    level: Level,
    noise: &N,
    start_coords: [f32; 1],
    dimensions: [usize; 1],
    destination: &mut [f32],
) {
    dispatch!(level, simd => sample_noise_1d_inner(simd, noise, start_coords, dimensions, destination))
}

pub fn sample_noise_2d<B: Sampleable<Coords = [f32; 2]>, N: Noise<B>>(
    level: Level,
    noise: &N,
    start_coords: [f32; 2],
    dimensions: [usize; 2],
    destination: &mut [f32],
) {
    dispatch!(level, simd => sample_noise_2d_inner(simd, noise, start_coords, dimensions, destination))
}

/// The SIMD width ntsc-rs's noise and shift loops use on this CPU (`S::f32s::N`
/// of `Level::new()`): 8 on AVX2, 4 on SSE4.2 / NEON / the fallback. The GPU
/// needs it: the noise coordinates advance in chunks of N, and shift_row uses
/// its FMA formula only inside whole N-blocks.
pub fn simd_lanes() -> usize {
    fn lanes<S: Simd>(_s: S) -> usize {
        S::f32s::N
    }
    dispatch!(Level::new(), simd => lanes(simd))
}

pub trait ScalarCoords: Copy {
    type SimdCoords<S: Simd>: Copy;
    fn splat_x<S: Simd>(self, simd: S) -> Self::SimdCoords<S>;
    fn map<S: Simd>(coords: Self::SimdCoords<S>, f: impl Fn(S::f32s) -> S::f32s) -> Self::SimdCoords<S>;
}

impl<const N: usize> ScalarCoords for [f32; N] {
    type SimdCoords<S: Simd> = [S::f32s; N];

    #[inline(always)]
    fn splat_x<S: Simd>(self, simd: S) -> Self::SimdCoords<S> {
        let mut coords = self.map(|c| S::f32s::splat(simd, c));
        for i in 0..S::f32s::N {
            coords[0][i] += i as f32;
        }
        coords
    }

    #[inline(always)]
    fn map<S: Simd>(coords: Self::SimdCoords<S>, f: impl Fn(<S as Simd>::f32s) -> <S as Simd>::f32s) -> Self::SimdCoords<S> {
        coords.map(f)
    }
}

pub trait Sampleable {
    type Coords: ScalarCoords;
    fn sample<S: Simd>(seed: i32, coords: <Self::Coords as ScalarCoords>::SimdCoords<S>) -> S::f32s;
}

pub struct Simplex1d;
impl Sampleable for Simplex1d {
    type Coords = [f32; 1];
    #[inline(always)]
    fn sample<S: Simd>(seed: i32, coords: <Self::Coords as ScalarCoords>::SimdCoords<S>) -> <S as Simd>::f32s {
        simplex_1d::<S>(coords, seed)
    }
}

pub struct Simplex2d;
impl Sampleable for Simplex2d {
    type Coords = [f32; 2];
    #[inline(always)]
    fn sample<S: Simd>(seed: i32, coords: <Self::Coords as ScalarCoords>::SimdCoords<S>) -> <S as Simd>::f32s {
        simplex_2d::<S>(coords, seed)
    }
}

// ---- noise/simplex.rs ----

#[inline(always)]
fn simplex_1d<S: Simd>(point: [S::f32s; 1], seed: i32) -> S::f32s {
    let ([[i0], [i1]], [[x0], [x1]]) = grid_1d::<S>(point);

    let gi0 = hash_1d::<S>((i0 ^ seed).bitcast());
    let gi1 = hash_1d::<S>((i1 ^ seed).bitcast());

    let t0 = (-x0).mul_add(x0, 1.0);
    let t20 = t0 * t0;
    let t40 = t20 * t20;
    let gx0 = gradient_1d::<S>(gi0);

    let t1 = (-x1).mul_add(x1, 1.0);
    let t21 = t1 * t1;
    let t41 = t21 * t21;
    let gx1 = gradient_1d::<S>(gi1);

    (t40 * gx0).mul_add(x0, t41 * gx1 * x1)
}

#[inline(always)]
fn gradient_1d<S: Simd>(hash: S::u32s) -> S::f32s {
    let h = hash >> 28;
    let v = ((h & 7) + 1).to_float::<S::f32s>();

    let h_and_8 = (h & 8).simd_eq(S::u32s::splat(hash.witness(), 0));
    v.copysign(h_and_8.bitcast::<S::f32s>())
}

#[inline(always)]
fn simplex_2d<S: Simd>(point: [S::f32s; 2], seed: i32) -> S::f32s {
    let ([[i0, j0], [i1, j1], [i2, j2]], [[x0, y0], [x1, y1], [x2, y2]]) = grid_2d::<S>(point);

    let seed = S::i32s::splat(i0.witness(), seed);
    let gi0 = pcg_3d::<S>([i0, j0, seed].map(|x| x.bitcast()))[0];
    let gi1 = pcg_3d::<S>([i1, j1, seed].map(|x| x.bitcast()))[0];
    let gi2 = pcg_3d::<S>([i2, j2, seed].map(|x| x.bitcast()))[0];

    let t0 = y0.mul_add(-y0, x0.mul_add(-x0, 0.5f32)).max(0.0f32);
    let t1 = y1.mul_add(-y1, x1.mul_add(-x1, 0.5)).max(0.0);
    let t2 = y2.mul_add(-y2, x2.mul_add(-x2, 0.5)).max(0.0);

    let t20 = t0 * t0;
    let t40 = t20 * t20;
    let t21 = t1 * t1;
    let t41 = t21 * t21;
    let t22 = t2 * t2;
    let t42 = t22 * t22;

    let [gx0, gy0] = gradient_2d::<S>(gi0);
    let g0 = gx0 * x0 + gy0 * y0;
    let n0 = t40 * g0;
    let [gx1, gy1] = gradient_2d::<S>(gi1);
    let g1 = gx1 * x1 + gy1 * y1;
    let n1 = t41 * g1;
    let [gx2, gy2] = gradient_2d::<S>(gi2);
    let g2 = gx2 * x2 + gy2 * y2;
    let n2 = t42 * g2;

    n0 + n1 + n2
}

#[inline(always)]
fn gradient_2d<S: Simd>(hash: S::u32s) -> [S::f32s; 2] {
    let h = hash & 7;

    let mask = h.simd_lt(S::u32s::splat(hash.witness(), 4));
    let _f1 = S::f32s::splat(hash.witness(), 1.0);
    let _f2 = S::f32s::splat(hash.witness(), 2.0);
    let x_magnitude = mask.select(_f1, _f2);
    let y_magnitude = mask.select(_f2, _f1);

    let h_and_1 = (h & 1).simd_eq(0);
    let h_and_2 = (h & 2).simd_eq(0);

    let gx = mask.select(h_and_1, h_and_2).select(x_magnitude, -x_magnitude);
    let gy = mask.select(h_and_2, h_and_1).select(y_magnitude, -y_magnitude);
    [gx, gy]
}

// ---- noise/grid.rs ----

#[inline(always)]
#[allow(clippy::type_complexity)]
fn grid_1d<S: Simd>([x]: [S::f32s; 1]) -> ([[S::i32s; 1]; 2], [[S::f32s; 1]; 2]) {
    let i = x.floor();
    let i0 = i.to_int();
    let i1 = i0 + 1;
    let x0 = x - i;
    let x1 = x0 - 1.0;
    ([[i0], [i1]], [[x0], [x1]])
}

#[inline(always)]
#[allow(clippy::type_complexity)]
fn grid_2d<S: Simd>([x, y]: [S::f32s; 2]) -> ([[S::i32s; 2]; 3], [[S::f32s; 2]; 3]) {
    let skew = skew_factor(2);
    let unskew = -unskew_factor(2);

    let s = (x + y) * skew;
    let ips = (x + s).floor();
    let jps = (y + s).floor();

    let i = ips.to_int();
    let j = jps.to_int();

    let t = S::f32s::float_from(i + j) * unskew;

    let x0: S::f32s = x - (ips - t);
    let y0: S::f32s = y - (jps - t);

    let i1 = x0.simd_ge(y0).bitcast::<S::i32s>();
    let j1 = y0.simd_gt(x0).bitcast::<S::i32s>();

    let x1 = x0 + S::f32s::float_from(i1) + unskew;
    let y1 = y0 + S::f32s::float_from(j1) + unskew;
    let x2 = x0 - 1.0 + 2.0 * unskew;
    let y2 = y0 - 1.0 + 2.0 * unskew;

    ([[i, j], [i - i1, j - j1], [i + 1, j + 1]], [[x0, y0], [x1, y1], [x2, y2]])
}

#[inline(always)]
fn skew_factor(dimension: usize) -> f32 {
    (((dimension + 1) as f32).sqrt() - 1.0) / dimension as f32
}

#[inline(always)]
fn unskew_factor(dimension: usize) -> f32 {
    ((1.0 / ((dimension + 1) as f32).sqrt()) - 1.0) / dimension as f32
}

// ---- noise/hash.rs ----

#[inline(always)]
fn hash_1d<S: Simd>(mut v: S::u32s) -> S::u32s {
    v = (v ^ 2747636419) * 2654435769;
    v = (v ^ (v >> 16)) * 2654435769;
    v = (v ^ (v >> 16)) * 2654435769;
    v
}

#[inline(always)]
fn pcg_3d<S: Simd>([mut vx, mut vy, mut vz]: [S::u32s; 3]) -> [S::u32s; 3] {
    vx = vx * 1664525 + 1013904223;
    vy = vy * 1664525 + 1013904223;
    vz = vz * 1664525 + 1013904223;

    vx += vy * vz;
    vy += vz * vx;
    vz += vx * vy;

    vx = vx ^ (vx >> 16);
    vy = vy ^ (vy >> 16);
    vz = vz ^ (vz >> 16);

    vx += vy * vz;
    vy += vz * vx;
    vz += vx * vy;

    [vx, vy, vz]
}
