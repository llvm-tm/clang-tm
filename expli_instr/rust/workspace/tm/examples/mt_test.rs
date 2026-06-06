use tm::{TmCell, transaction, tm_init, tm_exit, tm_init_thread, tm_exit_thread};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

fn main() {
    tm_init();
    
    let cell = Arc::new(TmCell::new(0i32));
    let stop = Arc::new(AtomicBool::new(false));
    
    let c = cell.clone();
    let s = stop.clone();
    let h = std::thread::spawn(move || {
        tm_init_thread();
        while !s.load(Ordering::Relaxed) {
            transaction(|tx| {
                let v = tx.read(&c);
                tx.write(&c, v + 1);
            });
        }
        tm_exit_thread();
    });
    
    std::thread::sleep(std::time::Duration::from_millis(100));
    stop.store(true, Ordering::Relaxed);
    h.join().unwrap();
    
    let final_val = cell.ptr();
    println!("Final value: {}", unsafe { *final_val });
    tm_exit();
    println!("PASS");
}
