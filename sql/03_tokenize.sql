-- ASCII-era tokenizer cases, updated to the M3 english analyzer: "the" is a
-- stopword (dropped); "quick"/"brown"/"fox" pass through (no inflection);
-- whitespace/punctuation normalized; empty input -> empty array.
CREATE EXTENSION bm25_native;
SELECT bm25_debug_tokenize('The Quick, BROWN fox!');
SELECT bm25_debug_tokenize('  multiple   spaces  ');
SELECT bm25_debug_tokenize('');
DROP EXTENSION bm25_native;
