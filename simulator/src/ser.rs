use crate::engine::SimState;

/// Save simulation state to a file (bincode-encoded).
pub fn save_checkpoint(state: &SimState, path: &str) -> Result<(), String> {
    let encoded = bincode::serialize(state)
        .map_err(|e| format!("serialize checkpoint: {}", e))?;
    std::fs::write(path, &encoded)
        .map_err(|e| format!("write checkpoint: {}", e))?;
    Ok(())
}

/// Load simulation state from a file.
pub fn load_checkpoint(path: &str) -> Result<SimState, String> {
    let data = std::fs::read(path)
        .map_err(|e| format!("read checkpoint: {}", e))?;
    let state: SimState = bincode::deserialize(&data)
        .map_err(|e| format!("deserialize checkpoint: {}", e))?;
    Ok(state)
}
