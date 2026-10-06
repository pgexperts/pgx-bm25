-- 00_smoke: extension installs and is visible.
CREATE EXTENSION bm25_native;
SELECT extname FROM pg_extension WHERE extname = 'bm25_native';
DROP EXTENSION bm25_native;
