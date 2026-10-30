use pulldown_cmark::Parser;
use std::{hint::black_box, time::Instant};
use xdnote_pulldown_markdown_spike::options;

fn sample(size: usize) -> String {
    let chunk = format!(
        "# 标题\n\n- [x] 笔记\n- [ ] 示例\n\n```text\n{}\n```\n\n",
        "x".repeat(2048)
    );
    let mut result = chunk.repeat(size / chunk.len());
    result.push_str(&"x".repeat(size - result.len()));
    result
}

fn main() {
    for size in [10_240, 1_048_576, 10_485_760] {
        let source = sample(size);
        assert_eq!(source.len(), size);
        let started = Instant::now();
        let count = black_box(Parser::new_ext(black_box(&source), options()).count());
        println!(
            "sample_bytes={size} events={count} parse_ms={:.3}",
            started.elapsed().as_secs_f64() * 1000.0
        );
    }
}
