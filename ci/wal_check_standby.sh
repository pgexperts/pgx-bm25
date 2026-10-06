#!/usr/bin/env bash
# Replay-check the hardening job's pg_regress cluster through a streaming standby
# (#309 CI-01).
#
# Role in the system. wal_consistency_checking makes every WAL record carry an image
# of each page it touches, and REDO compares the page it rebuilt against that image
# (generic WAL included: its rmgr has a mask function). A page dirtied outside
# Generic WAL, or a buffer missing from a record, then stops recovery with "FATAL:
# inconsistent page found". But the comparison happens only where WAL is REPLAYED.
# The TAP half gets that from its own crash and standby nodes (the workflow exports
# TEMP_CONFIG for them); the pg_regress half runs on one long-lived primary that is
# never recovered, so the setting alone would check nothing there. A standby that
# streams the primary's whole run replays every record as it is written, with no
# dependence on where checkpoints fall (a crash-restart replays only the WAL after
# the last one).
#
# LAG IS BOUNDED BY CONSTRUCTION. The run writes about 21 GB of WAL (every record
# carries check images), and a cassert standby replaying with the check on is far
# slower than the primary writing it: on a 2-core runner it fell more than 4 GB
# behind, its slot was invalidated ("wal_removed"), and the run failed with nothing
# wrong in the code. So the standby is SYNCHRONOUS and the primary runs with
# synchronous_commit = remote_apply: every commit waits until the standby has
# replayed it, so the standby trails the primary by at most the WAL of the
# transactions in flight -- a byte count fixed by the suites, not by how fast the
# runner is. WAL written without a commit that waits (VACUUM, CHECKPOINT, xid-less
# statements) is followed by the next commit, which does. The price is that the
# run proceeds at the standby's replay speed. The largest in-flight amount is one
# statement: sql/80_maintenance_interrupts' mi_dbg INSERT writes 4.8 GB of WAL in
# one transaction and its mi_pending INSERT 4.3 GB (pg_stat_statements, PG18
# cassert, 2026-10-05). The standby replays while the statement runs, so observed
# lag is lower (2.5 GB at full speed, 3.7 GB with replay throttled), but a stalled
# replay approaches the whole statement -- which is why the slot cap below is not
# the 4 GB it once was. verify prints the run's peak slot retention and warns past
# three quarters of the cap, so a suite that outgrows it shows up before it breaks
# the job.
#
# The slot stays (nothing the standby needs is recycled while it trails), and
# this script owns max_slot_wal_keep_size: SLOT_CAP while the standby is healthy,
# a backstop that only a double fault (dead standby AND dead watchdog) reaches,
# and 0 once the watchdog has released the primary, so a gone standby's slot is
# invalidated at the next checkpoint instead of pinning the run's WAL on disk.
#
# A synchronous standby that dies would hang every later commit until the job
# timeout. That is exactly the planted-defect case (replay stops on "inconsistent
# page found" and the standby shuts down), so a watchdog started with the standby
# releases the primary -- empties synchronous_standby_names -- when the standby
# server is gone, has had no replication connection to the primary for 30 s, or
# has not advanced its replay for 5 minutes with WAL outstanding. A standby that
# reconnects and is catching up (sync_state 'potential' until it does) is healthy:
# commits wait meanwhile, so the lag bound still holds, and the stall rule covers
# a catch-up that makes no progress. The run then finishes, and verify
# fails on the standby's log (or, with no evidence there, on the release itself).
#
#   start  STANDBY_DATA STANDBY_PORT STANDBY_LOG
#       Base-backs-up the RUNNING primary (reached through PGHOST/PGPORT) with a
#       physical slot, starts the standby, makes it the primary's synchronous
#       standby under remote_apply (failing unless pg_stat_replication confirms
#       it), and starts the watchdog (pid in STANDBY_LOG.watch.pid, output in
#       STANDBY_LOG.watch). The primary must already run with
#       wal_consistency_checking set, and must not set synchronous_commit,
#       synchronous_standby_names or max_slot_wal_keep_size on its command line
#       (see below).
#   verify STANDBY_PORT STANDBY_LOG
#       Stops the watchdog, waits for the standby to replay everything the primary
#       has written, then fails if the standby is gone, its log holds an
#       inconsistency, PANIC, TRAP or crashed process, or the watchdog had to
#       release the primary. Exits 1 with the evidence printed.
#   watch  STANDBY_DATA STANDBY_LOG
#       The watchdog loop (internal; `start` runs it in the background).
set -euo pipefail

die() { echo "::error::$*"; exit 1; }

APP=wcc_standby   # the standby's application_name, its slot, and the sync name
SLOT_CAP=8GB      # above the largest in-flight transaction (4.8 GB); see LAG IS BOUNDED
GONE_SECS=30      # no replication connection from the standby this long = gone
STALL_SECS=300    # replay not advancing this long with WAL outstanding = stuck

primary() { psql -XAtq -d postgres -c "$1"; }

case "${1:-}" in
start)
    [ $# -eq 4 ] || die "usage: $0 start STANDBY_DATA STANDBY_PORT STANDBY_LOG"
    sdata=$2 sport=$3 slog=$4
    wcc=$(primary 'SHOW wal_consistency_checking')
    # Canary: without the setting, records carry no check images and replay
    # compares nothing -- this whole arrangement would pass on any page bug.
    [ -n "$wcc" ] || die "primary runs with wal_consistency_checking empty -- the standby would check nothing"
    echo "primary wal_consistency_checking = $wcc"
    pg_basebackup -D "$sdata" -R -X stream -C -S "$APP" -d "port=$PGPORT host=$PGHOST"
    # cluster_name is the walreceiver's application_name when primary_conninfo
    # names none, and that is what synchronous_standby_names matches.
    # fsync=off as on the primary (whose setting is on its command line, which the
    # base backup does not copy): the run proceeds at the standby's speed, and a
    # throwaway standby gains nothing from durable writes.
    pg_ctl -D "$sdata" -l "$slog" -o "-p $sport -k $PGHOST -c cluster_name=$APP -c fsync=off" -w start
    # ALTER SYSTEM rather than -c on the primary's command line: the watchdog has
    # to be able to take synchronous_standby_names and the slot cap back, and a
    # command-line setting outranks postgresql.auto.conf. All three are reloadable.
    primary "ALTER SYSTEM SET max_slot_wal_keep_size = '$SLOT_CAP'"
    primary "ALTER SYSTEM SET synchronous_standby_names = '\"$APP\"'"
    primary "ALTER SYSTEM SET synchronous_commit = 'remote_apply'"
    primary 'SELECT pg_reload_conf()' >/dev/null
    # Canary: a name that matched no standby would leave commits hung, and a
    # setting that did not take would leave them unthrottled; require both.
    state=
    for _ in $(seq 1 60); do
        state=$(primary "SELECT sync_state FROM pg_stat_replication WHERE application_name = '$APP'")
        [ "$state" = sync ] && [ "$(primary 'SHOW synchronous_commit')" = remote_apply ] \
            && [ "$(primary 'SHOW max_slot_wal_keep_size')" = "$SLOT_CAP" ] && break
        state=
        sleep 1
    done
    [ "$state" = sync ] || die "standby $APP never became the primary's synchronous remote_apply standby"
    echo "standby $APP is synchronous; primary synchronous_commit = remote_apply"
    nohup bash "$0" watch "$sdata" "$slog" > "$slog.watch" 2>&1 < /dev/null &
    echo $! > "$slog.watch.pid"
    ;;
watch)
    [ $# -eq 3 ] || die "usage: $0 watch STANDBY_DATA STANDBY_LOG"
    sdata=$2 slog=$3
    gone=0 still=0 last_replay= peak=0 down=0
    while sleep 1; do
        # The primary going away ends the run (normal stop or a failure path):
        # there is nothing left to release. Five misses, so one failed query does
        # not leave the rest of the run unwatched.
        row=$(primary "SELECT coalesce(max(replay_lsn::text), ''),
                              count(*) > 0,
                              coalesce(bool_or(replay_lsn < sent_lsn), false),
                              (SELECT coalesce(max(pg_wal_lsn_diff(pg_current_wal_lsn(), restart_lsn)), 0)::bigint
                                 FROM pg_replication_slots WHERE slot_name = '$APP')
                         FROM pg_stat_replication WHERE application_name = '$APP'" 2>/dev/null) \
            || { down=$((down + 1)); [ $down -lt 5 ] && continue; exit 0; }
        down=0
        IFS='|' read -r replay connected behind held <<< "$row"
        if [ "$held" -gt "$peak" ]; then peak=$held; echo "$peak" > "$slog.peak"; fi
        reason=
        if ! pg_ctl -D "$sdata" status >/dev/null 2>&1; then
            reason="the standby server is gone"
        elif [ "$connected" != t ]; then
            gone=$((gone + 1)) still=0
            [ $gone -lt $GONE_SECS ] || reason="the standby has had no replication connection for $GONE_SECS s"
        else
            gone=0
            if [ "$behind" = t ] && [ "$replay" = "$last_replay" ]; then
                still=$((still + 1))
                [ $still -lt $STALL_SECS ] || reason="the standby's replay has not advanced past $replay for $STALL_SECS s"
            else
                still=0
            fi
            last_replay=$replay
        fi
        [ -n "$reason" ] || continue
        # Release every waiting commit, and stop the slot pinning WAL for a standby
        # that is not coming back; the run finishes and verify reports why.
        primary "ALTER SYSTEM SET synchronous_standby_names = ''" || true
        primary "ALTER SYSTEM SET max_slot_wal_keep_size = 0" || true
        primary 'SELECT pg_reload_conf()' >/dev/null || true
        echo "RELEASED: $reason ($(date -u '+%H:%M:%S') UTC)"
        exit 0
    done
    ;;
verify)
    [ $# -eq 3 ] || die "usage: $0 verify STANDBY_PORT STANDBY_LOG"
    sport=$2 slog=$3
    if [ -f "$slog.watch.pid" ]; then
        kill "$(cat "$slog.watch.pid")" 2>/dev/null || true
    fi
    released=$(grep -h '^RELEASED:' "$slog.watch" 2>/dev/null || true)
    peak=$(cat "$slog.peak" 2>/dev/null || echo 0)
    cap=$(primary "SELECT pg_size_bytes('$SLOT_CAP')")
    echo "peak WAL held by the standby's slot: $((peak / 1048576)) MB (cap $SLOT_CAP)"
    df -h "${RUNNER_TEMP:-$(dirname "$slog")}" || true
    if [ "$peak" -gt $((cap / 4 * 3)) ]; then
        echo "::warning::the standby's slot held $((peak / 1048576)) MB, over 3/4 of its $SLOT_CAP cap -- check whether a suite's largest transaction has grown, and raise SLOT_CAP in ci/wal_check_standby.sh"
    fi
    target=$(primary 'SELECT pg_current_wal_lsn()')
    ok=
    for _ in $(seq 1 600); do
        # A standby that died on an inconsistency refuses the connection, so a
        # failed query here is evidence, not a reason to abort the loop.
        if ! caught=$(psql -XAtq -p "$sport" -d postgres \
                -c "SELECT pg_last_wal_replay_lsn() >= '$target'::pg_lsn" 2>/dev/null); then
            break
        fi
        if [ "$caught" = t ]; then ok=1; break; fi
        sleep 1
    done
    bad=$(grep -E 'inconsistent page|PANIC:|TRAP:|terminated by signal|invalid page' "$slog" || true)
    # A release fails the check even when the standby later caught up: the run
    # was not throttled from then on, so the bound this script promises was lost.
    if [ -n "$bad" ] || [ -z "$ok" ] || [ -n "$released" ]; then
        echo "=== standby log ==="
        cat "$slog"
        [ -n "$bad" ] && die "WAL replay check failed on the standby: $(echo "$bad" | head -1)"
        [ -n "$released" ] && die "the watchdog released the primary from its synchronous standby: $released"
        die "standby did not replay to $target (down or stalled) -- see its log above"
    fi
    echo "standby replayed the whole run to $target with wal_consistency_checking: clean"
    ;;
*)
    die "usage: $0 start|verify|watch ..."
    ;;
esac
