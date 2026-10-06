-- 13_accum: drive the accumulator from SQL; assert dense docids, per-term df,
-- per-doc tf, and doclen accounting. ("quick quick fox" => tf(quick)=2.)
CREATE EXTENSION bm25_native;
SELECT * FROM bm25_debug_accum(ARRAY[
  'the quick brown fox',
  'the lazy brown dog',
  'quick quick fox'
]) ORDER BY term, local_docid;
DROP EXTENSION bm25_native;
