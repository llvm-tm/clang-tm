// ── Persistent Bank Test ──────────────────────────────────
// Tests sgl-persistent backend: bank account balances survive
// process restarts via file-backed MAP_SHARED memory.
//
// Auto-detect: first run initialises accounts; subsequent runs
// verify the total and perform additional transfers.
//
// Usage:
//   cargo run --release --no-default-features \
//     --features sgl-persistent --bin test_persist_bank
//
// Run multiple times to verify persistence.

use std::ffi::CString;
use std::path::Path;

const ACCOUNT_FILE: &str = "test_persist_accounts.bin";
const FILE_SIZE: i64 = 64 * 1024 * 1024;
const DATA_OFFSET: usize = 4096;
const NUM_ACCOUNTS: usize = 10;
const INITIAL_BALANCE: i64 = 100;
const EXPECTED_TOTAL: i64 = (NUM_ACCOUNTS as i64) * INITIAL_BALANCE;
const MAGIC: i64 = 0x50525354;

struct ShmRegion {
    ptr: *mut u8,
    size: usize,
}

impl ShmRegion {
    fn new(path: &str, size: usize) -> Self {
        if let Some(parent) = Path::new(path).parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        let cpath = CString::new(path).unwrap();
        let fd = unsafe { libc::open(cpath.as_ptr(), libc::O_RDWR | libc::O_CREAT, 0o644) };
        assert!(fd >= 0, "open({}) failed", path);
        let ret = unsafe { libc::ftruncate(fd, size as i64) };
        assert!(ret >= 0, "ftruncate failed");
        let ptr = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                size,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                fd,
                0,
            )
        };
        unsafe { libc::close(fd); }
        assert!(ptr != libc::MAP_FAILED, "mmap({}) failed", path);
        // Only zero the file on first creation (if it was just truncated from 0).
        // We detect this by checking if the file was empty before truncation.
        // For MAP_SHARED, the file offset 0 starts zeroed on creation.
        // We avoid re-zeroing on subsequent runs so persisted data survives.
        ShmRegion { ptr: ptr as *mut u8, size }
    }

    fn accounts(&self) -> *mut i64 {
        unsafe { self.ptr.add(DATA_OFFSET) as *mut i64 }
    }
}

impl Drop for ShmRegion {
    fn drop(&mut self) {
        unsafe { libc::munmap(self.ptr as *mut libc::c_void, self.size); }
    }
}

fn main() {
    tm::tm_init();
    let shm = ShmRegion::new(ACCOUNT_FILE, FILE_SIZE as usize);
    let accounts = shm.accounts();

    let magic = unsafe { accounts.read() };

    if magic == MAGIC {
        // ── Verify + transfer mode ──
        let src = fastrand::usize(0..NUM_ACCOUNTS);
        let dst = fastrand::usize(0..NUM_ACCOUNTS);
        let amount: i64 = 10;

        tm::transaction(|_tx| unsafe {
            let sb = accounts.add(1 + src).read();
            let db = accounts.add(1 + dst).read();
            assert!(sb >= amount, "insufficient balance in account {}", src);
            accounts.add(1 + src).write(sb - amount);
            accounts.add(1 + dst).write(db + amount);
        });

        tm::transaction(|_tx| {
            let mut total: i64 = 0;
            for i in 0..NUM_ACCOUNTS {
                total += unsafe { accounts.add(1 + i).read() };
            }
            assert_eq!(total, EXPECTED_TOTAL,
                "total mismatch: got {}, expected {}", total, EXPECTED_TOTAL);
            eprintln!("PASS: total={}, conserved after transfer", total);
        });
    } else {
        // ── Init mode ──
        tm::transaction(|_tx| unsafe {
            accounts.write(MAGIC);
            for i in 0..NUM_ACCOUNTS {
                accounts.add(1 + i).write(INITIAL_BALANCE);
            }
        });
        unsafe { libc::msync(shm.ptr as *mut libc::c_void, shm.size, libc::MS_SYNC); }
        eprintln!("INIT: {} accounts @ {} each, total={}",
            NUM_ACCOUNTS, INITIAL_BALANCE, EXPECTED_TOTAL);
        eprintln!("Run again to verify persistence + do a transfer");
    }

    tm::tm_exit();
}
