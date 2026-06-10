use serde::{Deserialize, Serialize};
use crate::event::{Event, EventKind};
use crate::queue::EventQueue;
use crate::lp::LpState;
use crate::memory::ShadowMemory;
use crate::checker::Checker;
use std::collections::HashMap;

/// Simulation engine: core DES loop.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SimState {
    pub clock: u64,
    pub queue: EventQueue,
    pub lps: HashMap<u32, LpState>,
    pub memory: ShadowMemory,
    pub checker: Checker,
    pub events_processed: u64,
    pub retire_watermark: u64,
}

impl SimState {
    pub fn new(seed: u64, threads: u32, tm_base: u64, tm_size: u64) -> Self {
        let mut lps = HashMap::new();
        for tid in 0..threads {
            lps.insert(tid, LpState::new(tid, seed));
        }
        SimState {
            clock: 0,
            queue: EventQueue::new(),
            lps,
            memory: ShadowMemory::new(tm_base, tm_size),
            checker: Checker::new(1000),
            events_processed: 0,
            retire_watermark: 0,
        }
    }

    pub fn load_events(&mut self, events: Vec<Event>) {
        for e in events {
            self.queue.push(e);
        }
    }

    fn dispatch(&mut self, event: &Event) {
        self.clock = event.timestamp.max(self.clock);
        let lp = self.lps.entry(event.thread_id)
            .or_insert_with(|| LpState::new(event.thread_id, 0));

        if let Err(reason) = self.checker.check(event) {
            eprintln!("VIOLATION at ts={} tid={}: {}", event.timestamp, event.thread_id, reason);
        }

        match &event.kind {
            EventKind::ThreadSpawn(child_id) => {
                let seed = event.timestamp.wrapping_add(*child_id as u64);
                self.lps.entry(*child_id).or_insert_with(|| LpState::new(*child_id, seed));
            }
            EventKind::TxBegin => {
                lp.in_tx = true;
                lp.tx_start_ts = event.timestamp;
                lp.retry_count = 0;
            }
            EventKind::TxEnd => {
                lp.in_tx = false;
                lp.read_set.clear();
                lp.write_set.clear();
            }
            EventKind::Read { addr, .. } => {
                if lp.in_tx {
                    lp.read_set.push(crate::lp::ReadEntry { addr: *addr, version: 0 });
                }
            }
            EventKind::Write { addr, val, .. } => {
                if lp.in_tx {
                    lp.write_set.push(crate::lp::WriteEntry { addr: *addr, old_val: 0, new_val: *val });
                }
            }
            EventKind::Alloc { addr, size } => {
                self.memory.alloc(*addr, *size);
            }
            EventKind::Free { addr } => {
                if let Err(e) = self.memory.free(*addr) {
                    eprintln!("MEMORY VIOLATION at ts={}: {}", event.timestamp, e);
                }
            }
            EventKind::Checkpoint => {
                self.retire_watermark = event.timestamp;
            }
            EventKind::Assert { cond, msg } => {
                if !cond {
                    eprintln!("ASSERTION FAILED at ts={} tid={}: {}", event.timestamp, event.thread_id, msg);
                }
            }
            EventKind::Log { msg } => {
                println!("[LOG ts={} tid={}] {}", event.timestamp, event.thread_id, msg);
            }
            EventKind::ThreadJoin(_) => {}
        }
    }

    /// Process events until queue is empty or max_events limit reached.
    pub fn run(&mut self, max_events: u64) -> Vec<Event> {
        let mut processed = Vec::new();
        while let Some(event) = self.queue.pop() {
            if max_events > 0 && processed.len() >= max_events as usize {
                self.queue.push(event);
                break;
            }
            self.dispatch(&event);
            processed.push(event);
            self.events_processed += 1;
        }
        processed
    }
}
