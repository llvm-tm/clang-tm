use tm::{TmCell, TmPrimitive, Transaction};

/// Fixed-capacity binary heap (min-heap) priority queue.
/// All operations require `&Transaction` and go through TM.
pub struct TMHeap<T: TmPrimitive, const MAX: usize> {
    data: Vec<TmCell<T>>,
    size: TmCell<i32>,
}

impl<T: TmPrimitive + PartialOrd + Copy, const MAX: usize> TMHeap<T, MAX> {
    pub fn new(init: T) -> Self {
        let mut data = Vec::with_capacity(MAX);
        for _ in 0..MAX {
            data.push(TmCell::new(init));
        }
        TMHeap { data, size: TmCell::new(0) }
    }

    pub fn len(&self, tx: &Transaction) -> i32 {
        tx.read(&self.size)
    }

    pub fn is_empty(&self, tx: &Transaction) -> bool {
        tx.read(&self.size) == 0
    }

    pub fn push(&self, tx: &Transaction, val: T) {
        let n = tx.read(&self.size);
        if (n as usize) >= MAX {
            return;
        }
        tx.write(&self.size, n + 1);
        let mut i = n as usize;
        tx.write(&self.data[i], val);
        // Sift up
        while i > 0 {
            let p = (i - 1) / 2;
            let pv = tx.read(&self.data[p]);
            let cv = tx.read(&self.data[i]);
            if cv < pv {
                tx.write(&self.data[p], cv);
                tx.write(&self.data[i], pv);
                i = p;
            } else {
                break;
            }
        }
    }

    pub fn pop(&self, tx: &Transaction) {
        let n = tx.read(&self.size);
        if n == 0 {
            return;
        }
        let n = (n - 1) as usize;
        tx.write(&self.size, n as i32);
        if n == 0 {
            return;
        }
        let last = tx.read(&self.data[n]);
        tx.write(&self.data[0], last);
        // Sift down
        let mut i = 0usize;
        loop {
            let mut smallest = i;
            let iv = tx.read(&self.data[i]);
            let l = 2 * i + 1;
            if l < n {
                let lv = tx.read(&self.data[l]);
                if lv < iv {
                    smallest = l;
                }
            }
            let sv = tx.read(&self.data[smallest]);
            let r = 2 * i + 2;
            if r < n {
                let rv = tx.read(&self.data[r]);
                if rv < sv {
                    smallest = r;
                }
            }
            if smallest != i {
                let sv = tx.read(&self.data[smallest]);
                tx.write(&self.data[smallest], iv);
                tx.write(&self.data[i], sv);
                i = smallest;
            } else {
                break;
            }
        }
    }

    pub fn top(&self, tx: &Transaction) -> T {
        tx.read(&self.data[0])
    }
}
