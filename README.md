# SQLite3 Groonga Bigram

A SQLite FTS5 extension that uses Groonga for bigram tokenization, supporting CJK (Chinese, Japanese, Korean) text search.

## Features

- Bigram tokenization using Groonga's TokenNgram
- Full-text search support for CJK characters
- SQLite FTS5 compatible

## Requirements

- sqlite >= 3.47.0 (compiled with FTS5) - Required for `fts5_tokenizer_v2` API support
- groonga

## Installation

### 1. Run setup script

```bash
./setup.sh
```

This will install SQLite with FTS5 support and Groonga.

### 2. Build

```bash
make extension
```

### 3. Test

```bash
./test.sh
```

## Usage

```bash
sqlite3 your.db
```

```sql
.load ./libbigram
CREATE VIRTUAL TABLE docs USING fts5(content, tokenize='bigram');
INSERT INTO docs VALUES('Hello World你好世界');
SELECT * FROM docs WHERE docs MATCH '你好';
```

## GitHub Actions

The project includes CI/CD automation:

- Runs tests on push to main
- Creates release on git tag push (e.g., `v1.0.0`)

```bash
git tag v1.0.0
git push --tags
```

This will automatically build and create a GitHub Release with `libbigram.so`.
