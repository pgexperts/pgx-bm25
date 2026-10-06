#!/usr/bin/env python3
"""Reference BM25 (Lucene variant) for cross-checking bm25_native.

Reads JSON {"docs": ["text", ...], "queries": ["q", ...], "k1":1.2, "b":0.75}
on stdin; writes JSON {"q": [[ [docidx, score], ... ], ...]} ranked desc by
score, ties broken by ascending document index (matching the C side's heap TID
order when rows are inserted in id order with no updates).

Tokenization mirrors the C bm25_tokenize: lowercase then keep ASCII alnum runs.
BM25 formula: Lucene "+1" IDF, k1=1.2, b=0.75 by default.
  idf   = ln(1 + (N - df + 0.5) / (df + 0.5))
  score = sum_t idf_t * tf*(k1+1) / (tf + k1*(1 - b + b*dl/avgdl))
"""
import sys
import json
import math
import re


def tokenize(s):
    """Lowercase then extract ASCII alphanumeric runs, matching bm25_tokenize.c."""
    return re.findall(r"[a-z0-9]+", s.lower())


def main():
    inp = json.load(sys.stdin)
    docs = [tokenize(d) for d in inp["docs"]]
    k1 = inp.get("k1", 1.2)
    b = inp.get("b", 0.75)
    N = len(docs)
    avgdl = sum(len(d) for d in docs) / N if N else 1.0

    # document frequency per term
    df = {}
    for d in docs:
        for t in set(d):
            df[t] = df.get(t, 0) + 1

    out = []
    for q in inp["queries"]:
        query_terms = set(tokenize(q))
        scores = []
        for i, d in enumerate(docs):
            dl = len(d)
            s = 0.0
            for qt in query_terms:
                if qt not in df:
                    continue
                tf = d.count(qt)
                if tf == 0:
                    continue
                idf = math.log(1.0 + (N - df[qt] + 0.5) / (df[qt] + 0.5))
                s += idf * (tf * (k1 + 1.0)) / (tf + k1 * (1.0 - b + b * dl / avgdl))
            if s > 0:
                scores.append([i, s])
        # sort descending by score, then ascending by doc index (tie-break)
        scores.sort(key=lambda x: (-x[1], x[0]))
        out.append(scores)

    json.dump({"q": out}, sys.stdout)


if __name__ == "__main__":
    main()
