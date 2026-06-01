use tm::{TmCell, transaction, tm_init, tm_exit};

fn main() {
    tm_init();
    
    let cell = TmCell::new(42i32);
    
    let result = transaction(|tx| {
        let val = tx.read(&cell);
        tx.write(&cell, val + 1);
        tx.read(&cell)
    });
    
    println!("Result: {}", result);
    assert_eq!(result, 43);
    
    transaction(|tx| {
        let val = tx.read(&cell);
        assert_eq!(val, 43);
    });
    
    println!("PASS: single-thread TM works");
    tm_exit();
}
