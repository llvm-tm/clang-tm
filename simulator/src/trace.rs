use serde::{Deserialize, Serialize};
use std::io::BufRead;
use crate::event::Event;

/// Load trace events from a JSONL file or stdin.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Trace {
    pub events: Vec<Event>,
}

impl Trace {
    pub fn from_jsonl<R: std::io::Read>(reader: R) -> Result<Self, String> {
        let mut events = Vec::new();
        let mut line_num = 0u64;
        for line in std::io::BufReader::new(reader).lines() {
            line_num += 1;
            let line = line.map_err(|e| format!("line {}: {}", line_num, e))?;
            let trimmed = line.trim();
            if trimmed.is_empty() || trimmed.starts_with('#') {
                continue;
            }
            let event: Event = serde_json::from_str(trimmed)
                .map_err(|e| format!("line {}: {}", line_num, e))?;
            events.push(event);
        }
        Ok(Trace { events })
    }

    pub fn from_jsonl_file(path: &str) -> Result<Self, String> {
        let file = std::fs::File::open(path)
            .map_err(|e| format!("cannot open trace '{}': {}", path, e))?;
        Self::from_jsonl(file)
    }

    pub fn to_jsonl<W: std::io::Write>(&self, writer: &mut W) -> Result<(), String> {
        for event in &self.events {
            let line = serde_json::to_string(event)
                .map_err(|e| format!("serialize: {}", e))?;
            writeln!(writer, "{}", line)
                .map_err(|e| format!("write: {}", e))?;
        }
        Ok(())
    }
}
