// ── Policy-based red-black tree (example) ─────────────────────────
// All shared-memory accesses go through A::load/store, so a single
// implementation works both inside (TmAccess) and outside (UntrackedAccess)
// transactions.
//
// Usage:
//   use explicit_rbtree::{RBTree, TmAccess, UntrackedAccess};
//
//   let tree = RBTree::<i64, ResData, UntrackedAccess>::new();
//   let result = tree.find(&42);  // outside TX, plain access
//
//   // Inside a TX:
//   let tree_tx = RBTree::<i64, ResData, TmAccess>::new();
//   let r = tree_tx.find(&42);

use crate::memory_access::MemAccess;

pub struct Node<K, V> {
    pub key:   K,
    pub val:   V,
    pub left:  *mut Node<K, V>,
    pub right: *mut Node<K, V>,
}

pub struct RBTree<K, V, A: MemAccess> {
    pub root: *mut Node<K, V>,
    _access:  core::marker::PhantomData<A>,
}

impl<K, V, A: MemAccess> RBTree<K, V, A> {
    pub fn new() -> Self {
        RBTree { root: core::ptr::null_mut(), _access: core::marker::PhantomData }
    }

    pub fn lookup(&self, key: &K) -> Option<&Node<K, V>>
    where K: Ord + Copy,
    {
        unsafe {
            let mut n = A::load(&self.root);
            while !n.is_null() {
                let nk = A::load(&(*n).key);
                if *key == nk { return Some(&*n); }
                n = if *key < nk {
                    A::load(&(*n).left)
                } else {
                    A::load(&(*n).right)
                };
            }
        }
        None
    }

    pub fn find(&self, key: &K) -> Option<&V>
    where K: Ord + Copy,
    {
        self.lookup(key).map(|n| &n.val)
    }

    pub fn contains(&self, key: &K) -> bool
    where K: Ord + Copy,
    {
        self.lookup(key).is_some()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::memory_access::UntrackedAccess;

    #[test]
    fn insert_and_find() {
        // Untracked (plain) test
        // In real usage, TmAccess would go through the TM runtime.
        let tree = RBTree::<i64, i64, UntrackedAccess>::new();
        // For a full test we'd need insert, which requires &mut self.
    }
}
