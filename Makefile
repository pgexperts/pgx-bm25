MODULE_big = bm25_native
OBJS = src/bm25_handler.o src/bm25_meta.o src/bm25_tokenize.o src/bm25_analyzer.o src/bm25_build.o src/bm25_segment.o src/bm25_scan.o src/bm25_scan_rank.o src/bm25_scan_match.o src/bm25_debug.o src/bm25_score.o src/bm25_seg_build.o src/bm25_accum.o src/bm25_seg_read.o src/bm25_seg_dict.o src/bm25_seg_chain.o src/bm25_seg_debug.o src/bm25_pending.o src/bm25_fsm.o src/bm25_merge.o src/bm25_keymap.o src/bm25_phrase.o src/bm25_snippet.o src/bm25_wand.o src/bm25_query.o src/bm25_upgrade.o src/bm25_selfuncs.o src/bm25_stats.o src/bm25_sb_wordclass.o src/bm25_page_lever.o
EXTENSION = bm25_native
DATA = bm25_native--1.0.sql
PGFILEDESC = "bm25_native - native BM25 ranked full-text index access method"
REGRESS = 00_smoke 01_create_am 02_meta 03_tokenize 04_build 05_match 06_insert 07_postings 08_score_math 09_rank_internal 10_order_by 11_rank_correctness 12_format 12_meta_v4 13_varbyte 13_accum 13_seg 14_pending_unit 14_pending 14_scale 15_multiseg 16_pending_ryw 17_delete 18_vacuum_reclaim 19_merge 20_merge_reclaim 21_retired_descriptors 22_format_v4 23_reloptions 24_fieldcfg 25_analyzer 26_fingerprint_gate 27_m3_acceptance 28_nonenglish_recall 29_multifield_decl 30_field_postings 31_bm25f_score 32_field_query 33_keymap 34_merge_multifield 35_m5_acceptance 36_positions 37_merge_positions 38_phrase 39_snippet 40_m4_acceptance 41_scored_scan_error 42_format_v5 43_wand_parity 44_wand_skip 45_m2b_acceptance 48_m6_builders 46_m6_boolean 47_m6_wildcard 49_m6_acceptance 50_orderby_dist 51_conformance 52_jsonb_filter_index_only 53_concurrent_scored_scans 54_score_key_keytype 55_format_compat 56_planner_estimates 57_ranking_quality 58_k1b_reloptions 59_term_length_cap 60_null_scankey 61_negative_idf 62_segcat_chain 63_debug_privileges 64_multi_scankey 65_multilexeme_tokens 66_scan_interrupts 67_wildcard_guc_privileges 68_pending_append_limits 69_decode_boundary 70_distance_volatility 71_maintenance_privileges 72_wand_tail_score_index 73_tokenizer_scratch 74_match_intersect 75_seg_chain_cursor 76_rescan_scratch 77_pending_doc_ceiling 78_trust_boundary_bounds 79_page_content_bounds 80_maintenance_interrupts 81_vacuum_reltuples 82_encoding_aware_tokens 83_query_limits_memory_bounds 84_snippet_escaping_and_char_budget 85_builder_search_path 86_errcodes_and_amvalidate 87_distance_scan_identity 88_pending_tail_space 89_pending_recycle_horizon 90_pending_zero_doc_drain 91_vacuum_seal_interaction 92_pending_stranded_continuation 93_am_handler_robustness 94_scored_scan_state_resolution 95_segment_pointer_bounds 96_wal_page_determinism 97_debug_probe_arguments 98_unbounded_input_loops 99_query_semantics 100_ingest_token_ceiling 101_stemmer_identity 102_ingest_fingerprint_gate 103_declaration_properties 104_build_memory_budget 105_merge_memory_budget 106_drain_memory_budget 107_per_run_positions 108_segcat_total_tokens 109_score_key_pending 110_chain_walk_consistency 111_wand_livedocs_per_reader 112_merge_append_distance 113_pending_sweep_extent 114_segcat_link_corruption 115_orderby_jsonb_validate 116_score_query_overloads 117_exhaustive_livedocs_checked 118_ranked_keys_forward_pass 119_dict_lookup_handoff 120_wand_tail_stats_pin 121_chain_page_image 122_reloption_registration 123_build_all_null 124_strict_chain_walkers 125_wand_straddle 126_key_identity_stamp 127_key_stamp_fallback 128_pending_chain_epoch 129_where_orderby_sql_semantics 130_offindex_field_scope 131_scored_scan_owner 132_dictionary_probe 133_pending_truncate_fsm 134_orphan_sweep_gate 136_page_lever 137_page_roles 138_retired_reclaim_validation 139_seg_walk 140_decode_value_bounds 142_readable_gate_rls 143_query_args_strict 144_reloption_opclass_surface 145_text_rhs_micro_parser 146_snippet_query_trees 147_accum_full_term_key 148_keymap_cancel_analyze_memory 149_hygiene_313 150_error_sites 151_block_header_offset
# TAP_TESTS=1: PGXS auto-discovers every t/*.pl (incl. the v4 crash/replica suites
# t/006_v4_crash.pl and t/007_v4_replica.pl). Do NOT add PROVE_TESTS -- it would
# shadow auto-discovery and silently drop suites. Use PROVE_FLAGS=... on the command
# line (e.g. PROVE_FLAGS='-v' make installcheck) for a verbose/subset run. To run
# the SQL suites ONLY, override on the command line with `TAP_TESTS=` (empty):
# PGXS gates every TAP rule on `ifdef TAP_TESTS`, and an empty command-line value
# beats this assignment. An empty `PROVE_TESTS=` does NOT do that -- PGXS treats it
# as unset and falls back to t/*.pl (#311 CI-08).
TAP_TESTS = 1
PG_CPPFLAGS = -I$(srcdir)/src
# -ffp-contract=off: the M2b block-max WAND path and the exhaustive scorer must
# produce BIT-IDENTICAL scores (the WAND==exhaustive parity contract, D1/D8 of the
# M2b design). Both paths accumulate `contrib = boost*termscore` then `acc += contrib`
# via a named intermediate as a rounding barrier; FMA contraction (fusing mul+add into
# one rounding) would defeat that barrier and could fuse the two structurally-identical
# expressions DIFFERENTLY across compilers, making bit-exact parity pass on macOS clang
# but fail on the CI Linux gcc/clang. Disabling contraction makes both paths two-rounding
# on every compiler. Verified free: adding this flag perturbs zero existing expected output.
PG_CFLAGS = -ffp-contract=off
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# PGXS tracks no header dependencies (the dev PG is built without --enable-depend,
# so autodepend is empty): without this, touching src/bm25_format.h or bm25.h --
# the on-disk struct definitions -- left every stale object linked into the new
# library, silently writing pages with a mismatched layout (#311 CI-09). Coarse on
# purpose: every object depends on every header, so a header edit rebuilds all of
# them. That costs a full rebuild but needs no per-file include analysis and cannot
# under-approximate. $(wildcard) runs after the PGXS include so $(srcdir) is set.
# The .bc bitcode targets (with_llvm=yes builds, pgxs.mk) carry the same layout
# hazard, so they get the same prerequisites.
$(OBJS) $(patsubst %.o,%.bc,$(OBJS)): $(wildcard $(srcdir)/src/*.h)

# Print the REGRESS list, for a CI leg that needs to run a SUBSET of it. Nothing
# uses it today: the macOS leg did, to exclude the two interrupt-latency suites,
# and #156 made those runner-speed-independent so that leg runs the full list
# again (see .github/workflows/ci.yml and ADR 0070's addendum). Kept because the
# next leg that needs a subset should use this rather than reinvent it, and
# because the reason it is a TARGET and not a grep in the workflow is the part
# worth preserving: make EVALUATES the variable, so a future reformat to a
# `REGRESS +=` continuation, or a backslash-continued line, silently defeats a
# line-anchored grep and leaves that leg running only the suites it happened to
# match -- green, and testing far less than it reports.
.PHONY: print-regress
print-regress:
	@echo $(REGRESS)
