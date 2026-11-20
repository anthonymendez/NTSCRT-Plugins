//! NTSCRT-Plugins addition to librashader-cache (MPL-2.0 OR GPL-3.0-only, as the rest
//! of the crate): the cache's storage, put in by rust/librashader-patch/apply.cmake in
//! place of the persy database (`cache::internal`), which lives in one file under the
//! user's cache folder (`%LOCALAPPDATA%\librashader`), takes an exclusive lock on it,
//! and deletes the whole file whenever it can't open it, e.g. while another thread or
//! process holds it: with the D3D11 runtime compiling a preset's passes on several
//! threads it lost most writes and was wiped again and again.
//!
//! Here each cached object is its own file, `<index>-<blake3 key>.bin`, in the folder
//! the host sets with `ntscrt_libra_set_cache_dir` (nothing is cached until it does):
//! - written to a unique temporary file in that folder and renamed over the final name,
//!   so readers (other threads, other processes) only ever see a whole file;
//! - checked when read (magic, length and a blake3 hash of the contents): a damaged or
//!   foreign file is ignored (and removed), and the object compiled again;
//! - any I/O error is a cache miss / an unsaved object, never a failure.
//! The host keys the folder (versions, GPU and driver) and caps its size.
use std::error::Error;
use std::ffi::CStr;
use std::fmt::Write as _;
use std::fs;
use std::io::Write as _;
use std::os::raw::c_char;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::RwLock;
use std::time::SystemTime;

static DIR: RwLock<Option<PathBuf>> = RwLock::new(None);
static SEQ: AtomicU64 = AtomicU64::new(0);
static HITS: AtomicU64 = AtomicU64::new(0);
static MISSES: AtomicU64 = AtomicU64::new(0);
static WRITES: AtomicU64 = AtomicU64::new(0);

const MAGIC: &[u8; 8] = b"NTSCRTc1";
const HEADER: usize = 8 + 8 + 32; // magic, payload length (LE), blake3(payload)
const MAX_OBJECT: usize = 64 << 20;

/// The folder cached objects go to; `None` turns the cache off.
pub fn set_cache_dir(dir: Option<PathBuf>) {
    *DIR.write().unwrap_or_else(|e| e.into_inner()) = dir;
}

/// Sets the cache folder (UTF-8). Null or empty turns the cache off. Returns 0.
#[no_mangle]
pub unsafe extern "C" fn ntscrt_libra_set_cache_dir(path: *const c_char) -> i32 {
    let dir = if path.is_null() {
        None
    } else {
        match unsafe { CStr::from_ptr(path) }.to_str() {
            Ok(s) if !s.is_empty() => Some(PathBuf::from(s)),
            _ => None,
        }
    };
    set_cache_dir(dir);
    0
}

/// Objects found in the cache, looked up but not found (or unusable), and written,
/// since the library loaded. Any pointer may be null.
#[no_mangle]
pub unsafe extern "C" fn ntscrt_libra_cache_stats(hits: *mut u64, misses: *mut u64, writes: *mut u64) {
    unsafe {
        if !hits.is_null() {
            *hits = HITS.load(Ordering::Relaxed);
        }
        if !misses.is_null() {
            *misses = MISSES.load(Ordering::Relaxed);
        }
        if !writes.is_null() {
            *writes = WRITES.load(Ordering::Relaxed);
        }
    }
}

pub(crate) struct Store {
    dir: PathBuf,
}

pub(crate) fn get_cache() -> Result<Store, Box<dyn Error>> {
    match DIR.read().unwrap_or_else(|e| e.into_inner()).as_ref() {
        Some(dir) => Ok(Store { dir: dir.clone() }),
        None => Err("shader cache off".into()),
    }
}

fn file_name(index: &str, key: &[u8]) -> String {
    let mut name = String::with_capacity(index.len() + 2 + key.len() * 2 + 4);
    name.push_str(index);
    name.push('-');
    for b in key {
        let _ = write!(name, "{b:02x}");
    }
    name.push_str(".bin");
    name
}

pub(crate) fn get_blob(cache: &Store, index: &str, key: &[u8]) -> Result<Option<Vec<u8>>, Box<dyn Error>> {
    let path = cache.dir.join(file_name(index, key));
    let data = match fs::read(&path) {
        Ok(data) => data,
        Err(e) => {
            MISSES.fetch_add(1, Ordering::Relaxed);
            return if e.kind() == std::io::ErrorKind::NotFound { Ok(None) } else { Err(e.into()) };
        }
    };
    let valid = data.len() >= HEADER
        && &data[..8] == MAGIC
        && u64::from_le_bytes(data[8..16].try_into().unwrap()) == (data.len() - HEADER) as u64
        && blake3::hash(&data[HEADER..]).as_bytes() == &data[16..HEADER];
    if !valid {
        MISSES.fetch_add(1, Ordering::Relaxed);
        let _ = fs::remove_file(&path);
        return Ok(None);
    }
    // Recently used: the host trims the folder oldest first.
    if let Ok(f) = fs::OpenOptions::new().write(true).open(&path) {
        let _ = f.set_modified(SystemTime::now());
    }
    HITS.fetch_add(1, Ordering::Relaxed);
    Ok(Some(data[HEADER..].to_vec()))
}

pub(crate) fn set_blob(cache: &Store, index: &str, key: &[u8], value: &[u8]) -> Result<(), Box<dyn Error>> {
    if value.len() > MAX_OBJECT {
        return Ok(());
    }
    let name = file_name(index, key);
    let path = cache.dir.join(&name);
    let tmp = cache.dir.join(format!(
        ".{name}.{}-{}.tmp",
        std::process::id(),
        SEQ.fetch_add(1, Ordering::Relaxed)
    ));
    let mut data = Vec::with_capacity(HEADER + value.len());
    data.extend_from_slice(MAGIC);
    data.extend_from_slice(&(value.len() as u64).to_le_bytes());
    data.extend_from_slice(blake3::hash(value).as_bytes());
    data.extend_from_slice(value);
    let written = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&tmp)
        .and_then(|mut f| f.write_all(&data))
        .and_then(|_| fs::rename(&tmp, &path));
    if let Err(e) = written {
        let _ = fs::remove_file(&tmp);
        return Err(e.into());
    }
    WRITES.fetch_add(1, Ordering::Relaxed);
    Ok(())
}

/// Called by the cache after a panic: nothing to delete here (a bad file is ignored
/// when read).
pub(crate) fn remove_cache() {}
