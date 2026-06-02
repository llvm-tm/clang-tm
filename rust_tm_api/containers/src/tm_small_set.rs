use tm::{TmCell, TmPrimitive, Transaction};

/// Fixed-capacity small set with O(n) linear scan.
/// All operations require a `&Transaction` and go through TM.
pub struct TMSmallSet<T: TmPrimitive, const MAX: usize> {
    data: Vec<TmCell<T>>,
    count: TmCell<i32>,
}

impl<T: TmPrimitive + PartialEq + Copy, const MAX: usize> TMSmallSet<T, MAX> {
    pub fn new(init: T) -> Self {
        let mut data = Vec::with_capacity(MAX);
        for _ in 0..MAX {
            data.push(TmCell::new(init));
        }
        TMSmallSet { data, count: TmCell::new(0) }
    }

    pub fn count(&self, tx: &Transaction) -> i32 {
        tx.read(&self.count)
    }

    pub fn contains(&self, tx: &Transaction, val: T) -> bool {
        let n = tx.read(&self.count);
        for i in 0..n {
            if tx.read(&self.data[i as usize]) == val {
                return true;
            }
        }
        false
    }

    pub fn insert(&self, tx: &Transaction, val: T) {
        if self.contains(tx, val) {
            return;
        }
        let n = tx.read(&self.count);
        if (n as usize) < MAX {
            tx.write(&self.data[n as usize], val);
            tx.write(&self.count, n + 1);
        }
    }

    pub fn erase(&self, tx: &Transaction, val: T) {
        let n = tx.read(&self.count);
        for i in 0..n {
            if tx.read(&self.data[i as usize]) == val {
                let last = tx.read(&self.data[(n - 1) as usize]);
                tx.write(&self.data[i as usize], last);
                tx.write(&self.count, n - 1);
                return;
            }
        }
    }

    pub fn for_each<F: FnMut(i32, T)>(&self, tx: &Transaction, mut f: F) {
        let n = tx.read(&self.count);
        for i in 0..n {
            f(i, tx.read(&self.data[i as usize]));
        }
    }
}
