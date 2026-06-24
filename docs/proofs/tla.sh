#!/usr/bin/env bash
#
# tla.sh — TLA+ Tool Convenience Script
#
# Usage:
#   ./tla.sh check BACKEND     — TLC safety check (e.g., ./tla.sh check TL2)
#   ./tla.sh check-all         — TLC safety check on all backends
#   ./tla.sh tla BACKEND       — Run pcal.trans (PlusCal → TLA+)
#   ./tla.sh tla-all           — pcal.trans on all PlusCal backends
#   ./tla.sh liveness BACKEND  — TLC liveness check (Spec_WF + PROPERTY)
#   ./tla.sh liveness-all      — Liveness on all backends
#   ./tla.sh sequential        — TLC on all PlusCal backends (Thread={1})
#   ./tla.sh large BACKEND     — TLC on large config (Addr={0,1})
#   ./tla.sh list              — List all backends with .tla + .cfg
#   ./tla.sh download-jar      — Download tla2tools.jar to /tmp/
#   ./tla.sh help              — Show this message
#
# Environment:
#   TLA2TOOLS_JAR  — path to tla2tools.jar (default: /tmp/tla2tools.jar)
#   TLC_OPTS       — extra TLC flags (e.g., -coverage 1)
#   JAVA           — Java binary (default: java)
#
# Examples:
#   ./tla.sh check TL2
#   TLC_OPTS="-coverage 1" ./tla.sh check-all
#   ./tla.sh tla TSXSim
#   ./tla.sh liveness-all

set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TLA2TOOLS_JAR="${TLA2TOOLS_JAR:-/tmp/tla2tools.jar}"
JAVA="${JAVA:-java}"
TLC_OPTS="${TLC_OPTS:-}"

# ── Auto-download if jar missing ──────────────────────────────────────
ensure_jar() {
    if [ ! -f "$TLA2TOOLS_JAR" ]; then
        echo "tla2tools.jar not found at $TLA2TOOLS_JAR"
        echo "Run '$(basename "$0") download-jar' to download it"
        exit 1
    fi
}

# ── List backends (all .tla files with .cfg, excluding liveness configs)
list_backends() {
    for f in "$DIR"/*.tla; do
        base="$(basename "$f" .tla)"
        [ -f "$DIR/$base.cfg" ] || continue
        # skip liveness-only backends (no safety .cfg)
        case "$base" in
            *-liveness) continue ;;
        esac
        echo "$base"
    done
}

# ── List liveness backends ────────────────────────────────────────────
list_liveness() {
    for f in "$DIR"/*-liveness.cfg; do
        base="$(basename "$f" -liveness.cfg)"
        echo "$base"
    done
}

# ── List PlusCal backends ─────────────────────────────────────────────
list_pluscal() {
    for f in "$DIR"/*.tla; do
        if grep -q '\-\-algorithm' "$f" 2>/dev/null; then
            basename "$f" .tla
        fi
    done
}

# ── Commands ──────────────────────────────────────────────────────────
cmd_check() {
    ensure_jar
    local backend="$1"
    echo "=== TLC (safety): $backend ==="
    "$JAVA" -cp "$TLA2TOOLS_JAR" tlc2.TLC -deadlock $TLC_OPTS \
        "$DIR/$backend.tla" -config "$DIR/$backend.cfg"
}

cmd_check_all() {
    for b in $(list_backends); do
        cmd_check "$b"
        echo
    done
}

cmd_tla() {
    ensure_jar
    local backend="$1"
    echo "=== pcal.trans: $backend ==="
    cp "$DIR/$backend.tla" "$DIR/$backend.tla.bak"
    "$JAVA" -cp "$TLA2TOOLS_JAR" pcal.trans -nocfg "$DIR/$backend" 2>&1 && \
        echo "Wrote $DIR/$backend.tla (backup at $DIR/$backend.tla.bak)" || \
        (mv "$DIR/$backend.tla.bak" "$DIR/$backend.tla"; false)
}

cmd_tla_all() {
    for b in $(list_pluscal); do
        [ "$b" = "TLAPS" ] && continue
        cmd_tla "$b"
    done
}

cmd_liveness() {
    ensure_jar
    local backend="$1"
    local cfg="$DIR/$backend-liveness.cfg"
    if [ ! -f "$cfg" ]; then
        echo "No liveness config for $backend (expected $cfg)"
        exit 1
    fi
    echo "=== TLC (liveness): $backend ==="
    "$JAVA" -cp "$TLA2TOOLS_JAR" tlc2.TLC $TLC_OPTS \
        "$DIR/$backend.tla" -config "$cfg"
}

cmd_liveness_all() {
    for b in $(list_liveness); do
        cmd_liveness "$b"
        echo
    done
}

cmd_sequential() {
    ensure_jar
    for b in $(list_pluscal); do
        local cfg="$DIR/$b-sequential.cfg"
        [ -f "$cfg" ] || continue
        echo "=== Sequential TLC: $b ==="
        "$JAVA" -cp "$TLA2TOOLS_JAR" tlc2.TLC -deadlock $TLC_OPTS \
            "$DIR/$b.tla" -config "$cfg" \
            | grep -E '(Error:|Model checking completed|^[0-9]+ states|Finished in)' || true
        echo
    done
}

cmd_large() {
    ensure_jar
    local backend="$1"
    local cfg="$DIR/$backend-large.cfg"
    if [ ! -f "$cfg" ]; then
        echo "No large config for $backend (expected $cfg)"
        exit 1
    fi
    echo "=== Large TLC: $backend ==="
    "$JAVA" -cp "$TLA2TOOLS_JAR" tlc2.TLC -deadlock $TLC_OPTS \
        "$DIR/$backend.tla" -config "$cfg"
}

cmd_download_jar() {
    local url="${TLA2TOOLS_URL:-https://github.com/tlaplus/tlaplus/releases/download/v1.6.0/tla2tools.jar}"
    echo "Downloading tla2tools.jar..."
    curl -sL -o "$TLA2TOOLS_JAR" "$url" && \
        echo "Downloaded to $TLA2TOOLS_JAR" || \
        (echo "Download failed"; rm -f "$TLA2TOOLS_JAR"; exit 1)
    # Verify: TLC starts and prints version
    # Use a subshell without pipefail to handle TLC's non-zero exit on -help
    if (set +o pipefail; "$JAVA" -cp "$TLA2TOOLS_JAR" tlc2.TLC -help 2>&1 | grep -q 'TLC2 Version'); then
        echo "Verified: TLC works"
    else
        echo "Warning: TLC verification failed (Java version may be incompatible)"
    fi
}

# ── Main dispatch ─────────────────────────────────────────────────────
case "${1:-help}" in
    check)
        cmd_check "${2:?Usage: $0 check BACKEND}"
        ;;
    check-all)
        cmd_check_all
        ;;
    tla)
        cmd_tla "${2:?Usage: $0 tla BACKEND}"
        ;;
    tla-all)
        cmd_tla_all
        ;;
    liveness)
        cmd_liveness "${2:?Usage: $0 liveness BACKEND}"
        ;;
    liveness-all)
        cmd_liveness_all
        ;;
    sequential)
        cmd_sequential
        ;;
    large)
        cmd_large "${2:?Usage: $0 large BACKEND}"
        ;;
    list)
        echo "=== Safety backends ==="
        list_backends
        echo
        echo "=== Liveness backends ==="
        list_liveness
        echo
        echo "=== PlusCal backends ==="
        list_pluscal
        ;;
    download-jar)
        cmd_download_jar
        ;;
    help|--help|-h)
        sed -n '/^# /,/^[^#]/p' "$0" | sed 's/^# //; /^$/d; \$d'
        ;;
    *)
        echo "Unknown command: $1"
        echo "Usage: $0 {check|tla|liveness|sequential|large|list|download-jar|help}"
        exit 1
        ;;
esac
