use std::collections::BinaryHeap;
use serde::{Deserialize, Serialize};
use crate::event::Event;
use std::cmp::Ordering;

/// Wrapper so BinaryHeap orders by ascending timestamp.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct OrdEvent(pub Event);

impl Ord for OrdEvent {
    fn cmp(&self, other: &Self) -> Ordering {
        // Reverse: BinaryHeap is a max-heap, so smaller timestamp = higher priority
        other.0.timestamp.cmp(&self.0.timestamp)
            .then_with(|| other.0.thread_id.cmp(&self.0.thread_id))
            .then_with(|| other.0.seq.cmp(&self.0.seq))
    }
}

impl PartialOrd for OrdEvent {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl PartialEq for OrdEvent {
    fn eq(&self, other: &Self) -> bool {
        self.0.timestamp == other.0.timestamp
            && self.0.thread_id == other.0.thread_id
            && self.0.seq == other.0.seq
    }
}

impl Eq for OrdEvent {}

/// Thread-safe event queue backed by BinaryHeap.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct EventQueue {
    heap: BinaryHeap<OrdEvent>,
}

impl EventQueue {
    pub fn new() -> Self {
        EventQueue {
            heap: BinaryHeap::new(),
        }
    }

    pub fn push(&mut self, event: Event) {
        self.heap.push(OrdEvent(event));
    }

    pub fn pop(&mut self) -> Option<Event> {
        self.heap.pop().map(|o| o.0)
    }

    pub fn peek(&self) -> Option<&Event> {
        self.heap.peek().map(|o| &o.0)
    }

    pub fn len(&self) -> usize {
        self.heap.len()
    }

    pub fn is_empty(&self) -> bool {
        self.heap.is_empty()
    }

    /// Drain all events with timestamp <= max_ts into a Vec (for checkpointing).
    pub fn drain_up_to(&mut self, max_ts: u64) -> Vec<Event> {
        let mut drained = Vec::new();
        while let Some(top) = self.peek() {
            if top.timestamp <= max_ts {
                drained.push(self.pop().unwrap());
            } else {
                break;
            }
        }
        drained
    }
}

impl From<Vec<Event>> for EventQueue {
    fn from(events: Vec<Event>) -> Self {
        let mut q = EventQueue::new();
        for e in events {
            q.push(e);
        }
        q
    }
}
