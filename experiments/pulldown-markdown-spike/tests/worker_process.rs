use pulldown_cmark::Parser;
use std::io::{BufRead, BufReader, Write};
use std::process::{Child, ChildStdin, Command, ExitStatus, Stdio};
use std::sync::mpsc::{self, Receiver};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};
use xdnote_pulldown_markdown_spike::{options, GenerationGate, PROBE_MAX_SOURCE_BYTES};

// Owns only the child created by this fixture. Failure or timeout still reaps it.
struct Probe {
    child: Child,
    input: Option<ChildStdin>,
    lines: Receiver<std::io::Result<String>>,
    reader: Option<JoinHandle<()>>,
}

impl Probe {
    fn start(hold: bool) -> Self {
        let mut command = Command::new(env!("CARGO_BIN_EXE_preview_probe"));
        if hold {
            command.arg("--test-hold-before-parse");
        }
        let mut child = command
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .expect("start isolated probe");
        let input = child.stdin.take().expect("probe stdin");
        let stdout = child.stdout.take().expect("probe stdout");
        let (sender, lines) = mpsc::channel();
        let reader = thread::spawn(move || {
            for line in BufReader::new(stdout).lines() {
                if sender.send(line).is_err() {
                    break;
                }
            }
        });
        Self {
            child,
            input: Some(input),
            lines,
            reader: Some(reader),
        }
    }

    fn frame(&mut self, generation: u64, length: u32, bytes: &[u8]) {
        let input = self.input.as_mut().expect("open stdin");
        input.write_all(&generation.to_le_bytes()).unwrap();
        input.write_all(&length.to_le_bytes()).unwrap();
        input.write_all(bytes).unwrap();
        input.flush().unwrap();
    }

    fn close_input(&mut self) {
        self.input.take();
    }

    fn resume(&mut self) {
        let input = self.input.as_mut().expect("open stdin");
        input.write_all(b"R").unwrap();
        input.flush().unwrap();
        self.close_input();
    }

    fn line(&self) -> String {
        self.lines
            .recv_timeout(Duration::from_secs(10))
            .expect("probe output timeout or closed pipe")
            .expect("read probe output")
    }

    fn wait(&mut self) -> ExitStatus {
        let started = Instant::now();
        loop {
            if let Some(status) = self.child.try_wait().expect("poll probe") {
                return status;
            }
            assert!(
                started.elapsed() < Duration::from_secs(10),
                "probe exit timeout"
            );
            thread::sleep(Duration::from_millis(10));
        }
    }
}

impl Drop for Probe {
    fn drop(&mut self) {
        self.close_input();
        if self.child.try_wait().ok().flatten().is_none() {
            let _ = self.child.kill();
        }
        let _ = self.child.wait();
        if let Some(reader) = self.reader.take() {
            let _ = reader.join();
        }
    }
}

#[test]
fn parser_process_receives_bounded_utf8_and_reports_only_summary() {
    let source = "# 甲🙂\r\n\r\n正文 &amp; 图片\n";
    let expected = Parser::new_ext(source, options()).count();
    let mut probe = Probe::start(false);
    probe.frame(7, source.len() as u32, source.as_bytes());
    probe.close_input();
    assert_eq!(probe.line(), format!("RESULT 7 {expected}"));
    assert!(probe.wait().success());
    assert!(probe.lines.recv_timeout(Duration::from_secs(1)).is_err());
}

#[test]
fn probe_rejects_oversized_and_invalid_utf8_frames() {
    let mut oversized = Probe::start(false);
    oversized.frame(1, (PROBE_MAX_SOURCE_BYTES + 1) as u32, &[]);
    oversized.close_input();
    assert!(!oversized.wait().success());

    let mut invalid = Probe::start(false);
    invalid.frame(2, 1, &[0xff]);
    invalid.close_input();
    assert!(!invalid.wait().success());

    let mut truncated = Probe::start(false);
    truncated.frame(3, 4, b"ab");
    truncated.close_input();
    assert!(!truncated.wait().success());
}

#[test]
fn later_result_wins_even_if_an_older_process_finishes_last() {
    let mut gate = GenerationGate::default();
    let old = gate.begin().unwrap();
    let mut older = Probe::start(true);
    older.frame(old, 3, b"old");
    assert_eq!(older.line(), "READY");

    let new = gate.begin().unwrap();
    let mut newer = Probe::start(false);
    newer.frame(new, 3, b"new");
    newer.close_input();
    assert_eq!(newer.line(), format!("RESULT {new} 3"));
    assert!(newer.wait().success());
    assert!(gate.accept(new));
    assert!(!gate.accept(new)); // A duplicate cannot be applied twice.

    older.resume();
    assert_eq!(older.line(), format!("RESULT {old} 3"));
    assert!(older.wait().success());
    assert!(!gate.accept(old));
}

#[test]
fn cancellation_reaps_only_the_controlled_child_and_rejects_its_generation() {
    let mut gate = GenerationGate::default();
    let generation = gate.begin().unwrap();
    let mut probe = Probe::start(true);
    probe.frame(generation, 3, b"old");
    assert_eq!(probe.line(), "READY");
    gate.cancel();
    probe.child.kill().expect("terminate controlled child");
    assert!(!probe.wait().success());
    assert!(!gate.accept(generation));
    assert!(probe.lines.recv_timeout(Duration::from_secs(1)).is_err());

    // A cancelled generation does not prevent a later independent job.
    let next = gate.begin().unwrap();
    assert!(next > generation);
    let mut replacement = Probe::start(false);
    replacement.frame(next, 3, b"new");
    replacement.close_input();
    assert_eq!(replacement.line(), format!("RESULT {next} 3"));
    assert!(replacement.wait().success());
    assert!(gate.accept(next));
}
