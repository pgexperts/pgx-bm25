---
id: 0001
title: PURPOSE
date: 2026-07-11
status: Accepted
summary: The native PostgreSQL BM25 index access method — @@@/&@@ operators, BM25F, phrase/proximity, snippets, and M6 boolean+wildcard jsonb query trees — with all state in the index relation so crash recovery and replication come from core.
---

# 0001. PURPOSE

`pg_bm25_index` is a PostgreSQL **custom index access method** (`USING bm25`) implementing
Okapi BM25 ranked full-text search natively inside the database. It exposes a `@@@` match
operator and a `&@@` order-by-operator so `ORDER BY col &@@ 'query' LIMIT k` becomes an
ordered index scan (`amcanorderbyop`) with no Sort node. It supports multi-column **BM25F**
ranking, per-field query scoping (`field:term`), and returning a user `key_field` (M5);
positional **phrase and proximity** search (`"a b"`, `"a b"~n` unordered W/n, `"a b"~>n`
ordered PRE/n) plus `bm25_snippet` highlighting (M4); and, as of **M6**, structured
**boolean + wildcard query trees** — `bm25_boolean(must/should/must_not)`, `bm25_wildcard`,
`bm25_phrase`, `bm25_boost`, `bm25_term`/`bm25_match_terms` builders that compose a **jsonb**
query object evaluated via `(text,jsonb)` `@@@`/`&@@` overloads. The goal is production-grade
relevance ranking that lives entirely in the index relation's own pages, so crash recovery
and physical replication are inherited from core PostgreSQL.
