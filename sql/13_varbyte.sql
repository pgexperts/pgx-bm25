-- 13_varbyte: encode then decode each value; assert identity and byte counts.
CREATE EXTENSION bm25_native;
SELECT v, (bm25_debug_varbyte_roundtrip(v)).*
  FROM (VALUES (0::int8),(1),(127),(128),(16383),(16384),(2097151),(2097152),(4294967295)) AS t(v);
DROP EXTENSION bm25_native;
