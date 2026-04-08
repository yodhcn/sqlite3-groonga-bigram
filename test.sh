#!/bin/bash

set -e

echo "=== Bigram Tokenizer End-to-End Test ==="
echo ""

echo "Cleaning up previous build..."
rm -f libbigram.so test.db
echo "  Done"
echo ""

echo "Building extension..."
make extension
echo "  Done"
echo ""

echo "Running all tests in a single session..."
sqlite3 test.db <<'EOF'
.load ./libbigram
CREATE VIRTUAL TABLE docs USING fts5(content, tokenize='bigram');
INSERT INTO docs VALUES('Hello World你好世界');
INSERT INTO docs VALUES('World Hello世界你好');
CREATE VIRTUAL TABLE vocab USING fts5vocab(docs, row);

SELECT '=== All tokens in index ===' as result;
SELECT * FROM vocab;

SELECT '=== Query: 你好 世界 ===' as result;
SELECT highlight(docs, 0, '<b>', '</b>') FROM docs WHERE docs MATCH '你好 世界';

SELECT '=== Query: Hello世* ===' as result;
SELECT highlight(docs, 0, '<b>', '</b>') FROM docs WHERE docs MATCH 'Hello世*';

SELECT '=== Query: 好 ===' as result;
SELECT highlight(docs, 0, '<b>', '</b>') FROM docs WHERE docs MATCH '好';

SELECT '=== Query: 你好 ===' as result;
SELECT highlight(docs, 0, '<b>', '</b>') FROM docs WHERE docs MATCH '你好';

SELECT '=== Query: 你好* ===' as result;
SELECT highlight(docs, 0, '<b>', '</b>') FROM docs WHERE docs MATCH '你好*';

SELECT '=== Query: 你好世 ===' as result;
SELECT highlight(docs, 0, '<b>', '</b>') FROM docs WHERE docs MATCH '你好世';

SELECT '=== Query: "Hello World" ===' as result;
SELECT highlight(docs, 0, '<b>', '</b>') FROM docs WHERE docs MATCH '"Hello World"';

SELECT '=== Query: world好世 ===' as result;
SELECT highlight(docs, 0, '<b>', '</b>') FROM docs WHERE docs MATCH 'world好世';

SELECT '=== Query: world你好 ===' as result;
SELECT highlight(docs, 0, '<b>', '</b>') FROM docs WHERE docs MATCH 'world你好';
EOF
echo ""
echo "=== All tests passed! ==="

echo "Cleaning up..."
rm -f libbigram.so test.db
echo "  Done"
echo ""
