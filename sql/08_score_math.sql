CREATE EXTENSION bm25_native;
SELECT round(bm25_debug_score(2, 1, 2, 3, 3.5, 1.2, 0.75)::numeric, 6) AS score;
SELECT round(bm25_debug_score(2, 1, 1, 4, 3.5, 1.2, 0.75)::numeric, 6) AS score2;
DROP EXTENSION bm25_native;
