use serde::{Deserialize, Serialize};
use std::collections::HashMap;

/// Shadow memory tracks TM allocations and detects address-space violations.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ShadowMemory {
    /// TM region base and size (from tm_region_allocator).
    pub tm_region_base: u64,
    pub tm_region_size: u64,

    /// Active allocations: addr -> (size, is_freed).
    allocations: HashMap<u64, Allocation>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Allocation {
    pub size: u64,
    pub is_freed: bool,
}

impl ShadowMemory {
    pub fn new(base: u64, size: u64) -> Self {
        ShadowMemory {
            tm_region_base: base,
            tm_region_size: size,
            allocations: HashMap::new(),
        }
    }

    pub fn is_tm_address(&self, addr: u64) -> bool {
        addr >= self.tm_region_base && addr < self.tm_region_base + self.tm_region_size
    }

    pub fn alloc(&mut self, addr: u64, size: u64) {
        self.allocations.insert(addr, Allocation { size, is_freed: false });
    }

    pub fn free(&mut self, addr: u64) -> Result<bool, String> {
        match self.allocations.get_mut(&addr) {
            None => Err(format!("free of unallocated address 0x{:x}", addr)),
            Some(a) if a.is_freed => Err(format!("double-free of address 0x{:x}", addr)),
            Some(a) => {
                a.is_freed = true;
                Ok(true)
            }
        }
    }

    pub fn is_valid_ptr(&self, addr: u64) -> bool {
        self.allocations.get(&addr).map_or(false, |a| !a.is_freed)
    }
}
