//! A bounded parser-process fixture. It is not an xdnote preview renderer.

use pulldown_cmark::Parser;
use std::io::{self, Read, Write};
use xdnote_pulldown_markdown_spike::{options, PROBE_MAX_SOURCE_BYTES};

fn run() -> io::Result<()> {
    let arguments: Vec<_> = std::env::args_os().skip(1).collect();
    let hold = match arguments.as_slice() {
        [] => false,
        [flag] if flag == "--test-hold-before-parse" => true,
        _ => {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "unknown probe mode",
            ))
        }
    };
    let mut input = io::stdin().lock();
    let mut output = io::stdout().lock();
    let mut header = [0_u8; 12];
    input.read_exact(&mut header)?;
    let generation = u64::from_le_bytes(header[..8].try_into().unwrap());
    let length = u32::from_le_bytes(header[8..].try_into().unwrap()) as usize;
    if length > PROBE_MAX_SOURCE_BYTES {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "fixture input too large",
        ));
    }
    let mut bytes = vec![0; length];
    input.read_exact(&mut bytes)?;
    let source = std::str::from_utf8(&bytes)
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid UTF-8"))?;

    // Only the controlled test mode waits. It makes cancellation and result
    // ordering deterministic without claiming interruption inside the parser.
    if hold {
        writeln!(output, "READY")?;
        output.flush()?;
        let mut resume = [0];
        input.read_exact(&mut resume)?;
        if resume != [b'R'] {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "invalid resume marker",
            ));
        }
    }
    let events = Parser::new_ext(source, options()).count();
    writeln!(output, "RESULT {generation} {events}")?;
    output.flush()
}

fn main() {
    if let Err(error) = run() {
        eprintln!("preview probe: {error}");
        std::process::exit(1);
    }
}
