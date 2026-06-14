// ── Backend abstraction ─────────────────────────────────
// Runtime dispatching between TM backends in simulation mode.
// Each registered backend exports the same simulation API.

/// Available TM backends.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Backend {
    Norec,
    Tl2,
}

impl Backend {
    pub fn from_name(name: &str) -> Option<Self> {
        match name {
            "norec" | "no-rec" => Some(Backend::Norec),
            "tl2" | "TL2" => Some(Backend::Tl2),
            _ => None,
        }
    }

    pub fn name(&self) -> &'static str {
        match self {
            Backend::Norec => "norec",
            Backend::Tl2 => "tl2",
        }
    }

    /// Initialize the TM runtime.
    pub fn init(&self) {
        match self {
            Backend::Norec => runtime_norec::tm_init(),
            Backend::Tl2 => runtime_tl2::tm_init(),
        }
    }

    /// Initialize a (simulated) thread.
    pub fn init_thread(&self) {
        match self {
            Backend::Norec => runtime_norec::tm_init_thread(),
            Backend::Tl2 => runtime_tl2::tm_init_thread(),
        }
    }

    /// Begin a transaction.
    pub fn begin(&self) {
        match self {
            Backend::Norec => runtime_norec::tm_begin(),
            Backend::Tl2 => runtime_tl2::tm_begin(),
        }
    }

    /// Commit a transaction. Returns true on success.
    pub fn commit(&self) -> bool {
        match self {
            Backend::Norec => runtime_norec::tm_commit(),
            Backend::Tl2 => runtime_tl2::tm_commit(),
        }
    }

    /// Abort the current transaction.
    pub fn abort(&self) {
        match self {
            Backend::Norec => runtime_norec::tm_abort(),
            Backend::Tl2 => runtime_tl2::tm_abort(),
        }
    }

    /// Set the simulated thread ID.
    pub fn sim_set_thread_id(&self, id: u64) {
        match self {
            Backend::Norec => runtime_norec::sim::set_thread_id(id),
            Backend::Tl2 => runtime_tl2::sim::set_thread_id(id),
        }
    }

    /// Clear the simulated thread ID.
    pub fn sim_clear_thread_id(&self) {
        match self {
            Backend::Norec => runtime_norec::sim::clear_thread_id(),
            Backend::Tl2 => runtime_tl2::sim::clear_thread_id(),
        }
    }

    /// Reset all simulated thread state.
    pub fn sim_reset(&self) {
        match self {
            Backend::Norec => runtime_norec::sim::reset(),
            Backend::Tl2 => runtime_tl2::sim::reset(),
        }
    }

    /// Typed transactional reads.
    pub fn read_u8(&self, addr: *mut u8) -> u8 {
        match self {
            Backend::Norec => runtime_norec::tm_read_u8(addr),
            Backend::Tl2 => runtime_tl2::tm_read_u8(addr),
        }
    }
    pub fn read_u16(&self, addr: *mut u16) -> u16 {
        match self {
            Backend::Norec => runtime_norec::tm_read_u16(addr),
            Backend::Tl2 => runtime_tl2::tm_read_u16(addr),
        }
    }
    pub fn read_u32(&self, addr: *mut u32) -> u32 {
        match self {
            Backend::Norec => runtime_norec::tm_read_u32(addr),
            Backend::Tl2 => runtime_tl2::tm_read_u32(addr),
        }
    }
    pub fn read_u64(&self, addr: *mut u64) -> u64 {
        match self {
            Backend::Norec => runtime_norec::tm_read_u64(addr),
            Backend::Tl2 => runtime_tl2::tm_read_u64(addr),
        }
    }

    /// Typed transactional writes.
    pub fn write_u8(&self, addr: *mut u8, val: u8) {
        match self {
            Backend::Norec => runtime_norec::tm_write_u8(addr, val),
            Backend::Tl2 => runtime_tl2::tm_write_u8(addr, val),
        }
    }
    pub fn write_u16(&self, addr: *mut u16, val: u16) {
        match self {
            Backend::Norec => runtime_norec::tm_write_u16(addr, val),
            Backend::Tl2 => runtime_tl2::tm_write_u16(addr, val),
        }
    }
    pub fn write_u32(&self, addr: *mut u32, val: u32) {
        match self {
            Backend::Norec => runtime_norec::tm_write_u32(addr, val),
            Backend::Tl2 => runtime_tl2::tm_write_u32(addr, val),
        }
    }
    pub fn write_u64(&self, addr: *mut u64, val: u64) {
        match self {
            Backend::Norec => runtime_norec::tm_write_u64(addr, val),
            Backend::Tl2 => runtime_tl2::tm_write_u64(addr, val),
        }
    }
}
