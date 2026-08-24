// graph_lang_rust: Rust language plugin for the graph analyzer.
//
// This module provides:
//   - TomlDocument: minimal Cargo.toml reader
//   - rustlex: Rust-dialect lexer (blanking, leading doc)
//   - RustFileItems: per-file scan result
//   - RustAnalyzer: discovers crates, derives modules, scans items
//
// Links graph_lang (the language-neutral foundation).
//
// TODO: Integrate with graph_lang::LanguageAnalyzer interface.
// TODO: Integrate with graph_lang::ir::Repo for intermediate representation.
// TODO: Integrate with graph_lang::RepoFileIndex for file indexing.

pub mod toml_document;
pub mod rustlex;
pub mod rust_file_items;
pub mod scan_rust_file;
pub mod rust_analyzer;

// Re-export for convenience
pub use toml_document::TomlDocument;
pub use rust_file_items::RustFileItems;
pub use rust_analyzer::RustAnalyzer;
