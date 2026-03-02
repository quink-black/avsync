#!/opt/homebrew/bin/bash
# =============================================================================
# AV Auto-Sync: Full comparison test with both detectors
# Optimized: per-sample timeout, lightweight output parsing
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

AVSYNC="${AVSYNC:-$PROJECT_DIR/build/bin/avsync}"
INPUT_DIR="${AVSYNC_INPUT_DIR:-/Users/quink/Movies/avsync/test_samples}"
REPORT_DIR="${AVSYNC_REPORT_DIR:-$(dirname "$INPUT_DIR")/test_reports}"
OUTPUT_DIR="${AVSYNC_OUTPUT_DIR:-$(dirname "$INPUT_DIR")/test_output}"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
REPORT="$REPORT_DIR/full_comparison_${TIMESTAMP}.txt"
CSV_REPORT="$REPORT_DIR/full_comparison_${TIMESTAMP}.csv"
DETAIL_DIR="$REPORT_DIR/details_comparison_${TIMESTAMP}"
mkdir -p "$OUTPUT_DIR" "$REPORT_DIR" "$DETAIL_DIR"

TOLERANCE=40
THRESH=40
WINDOW=10.0
STEP=5.0
TIMEOUT=60  # Per-sample timeout in seconds

DETECTORS=("onset_align" "syncnet")

parse_injection() {
    local base="$1"
    case "$base" in
        *pos_20ms) echo 20;;  *neg_30ms) echo -30;; *pos_40ms) echo 40;;
        *pos_50ms) echo 50;;  *neg_45ms) echo -45;; *pos_80ms) echo 80;;
        *pos_100ms) echo 100;; *neg_100ms) echo -100;; *pos_150ms) echo 150;;
        *pos_200ms) echo 200;; *neg_250ms) echo -250;; *pos_500ms) echo 500;;
        *pos_1000ms) echo 1000;; *neg_1000ms) echo -1000;;
        *pos_2000ms) echo 2000;; *neg_2000ms) echo -2000;;
        *0ms) echo 0;; *) echo 0;;
    esac
}

parse_detection() {
    local output="$1"
    local det=""
    local global_ofs=$(echo "$output" | grep "Global constant offset" | grep -oE '[-]?[0-9]+\.[0-9]+' | head -1)
    if [ -n "$global_ofs" ]; then
        det="$global_ofs"
    else
        local ofs_list=$(echo "$output" | grep -A200 "Correction Decisions" | grep "YES" | awk '{print $3}')
        if [ -n "$ofs_list" ]; then
            local s=0 c=0
            for v in $ofs_list; do s=$(echo "$s + $v" | bc); c=$((c+1)); done
            [ $c -gt 0 ] && det=$(echo "scale=1; $s / $c" | bc)
        fi
    fi
    if [ -z "$det" ]; then
        if echo "$output" | grep -q "no corrections\|0 segments corrected"; then
            det="0"
        else
            det="N/A"
        fi
    fi
    echo "$det"
}

# Also extract average confidence
parse_avg_confidence() {
    local output="$1"
    local confs=$(echo "$output" | grep -oE 'confidence=[0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+')
    if [ -n "$confs" ]; then
        local s=0 c=0
        for v in $confs; do s=$(echo "$s + $v" | bc); c=$((c+1)); done
        [ $c -gt 0 ] && echo "$(echo "scale=2; $s / $c" | bc)" && return
    fi
    echo "0.00"
}

evaluate() {
    local inj="$1" det="$2"
    local ai=$(echo "${inj#-}" | bc)
    local ad=$(echo "${det#-}" | bc 2>/dev/null || echo 0)

    if [ "$det" = "N/A" ]; then echo "ERR|N/A|no output"; return; fi
    if [ "$det" = "TIMEOUT" ]; then echo "ERR|N/A|timeout"; return; fi

    if [ "$inj" = "0" ]; then
        if (( $(echo "$ad < $TOLERANCE" | bc -l) )); then
            echo "PASS|0|baseline ok"
        else
            echo "FAIL|$det|false positive"
        fi
        return
    fi

    if (( $(echo "$ai < $THRESH" | bc -l) )); then
        if (( $(echo "$ad < $THRESH" | bc -l) )); then
            echo "PASS|0|below thresh ok"
        else
            echo "FAIL|$det|below thresh but corrected"
        fi
        return
    fi

    if [ "$det" != "0" ]; then
        local em=$(echo "scale=1; $det - ($inj)" | bc)
        local ae=$(echo "${em#-}" | bc)
        if (( $(echo "$ae <= $TOLERANCE" | bc -l) )); then
            echo "PASS|$em|"
        else
            echo "FAIL|$em|err>±${TOLERANCE}ms"
        fi
    else
        echo "FAIL|N/A|missed"
    fi
}

# ====================== HEADER ======================
{
echo "============================================================"
echo " AV Auto-Sync: Full Dual-Detector Comparison Report"
echo " $(date)"
echo " Tolerance: ±${TOLERANCE}ms | Threshold: ${THRESH}ms"
echo " Segment: ${WINDOW}s window / ${STEP}s step"
echo " Input: ${INPUT_DIR}"
echo " Timeout: ${TIMEOUT}s per sample"
echo "============================================================"
echo ""
} | tee "$REPORT"

echo "source,offset_category,injected_ms,detector,detected_ms,error_ms,avg_confidence,result,notes" > "$CSV_REPORT"

# Per-detector stats
OA_TOTAL=0; OA_PASS=0; OA_FAIL=0; OA_ERR=0
SN_TOTAL=0; SN_PASS=0; SN_FAIL=0; SN_ERR=0

SRC_STATS_DIR=$(mktemp -d)

# ====================== RUN TESTS ======================
for det in "${DETECTORS[@]}"; do
    {
    echo ""
    echo "╔══════════════════════════════════════════════════════════╗"
    printf "║  Detector: %-46s║\n" "$det"
    echo "╚══════════════════════════════════════════════════════════╝"
    echo ""
    printf "%-42s | %7s | %7s | %7s | %5s | %6s | %s\n" "Sample" "Inject" "Detect" "Error" "Conf" "Result" "Notes"
    echo "------------------------------------------------------------------------------------------------------------------------------"
    } | tee -a "$REPORT"

    for f in "$INPUT_DIR"/*__*.mp4; do
        [ -f "$f" ] || continue
        BASE="$(basename "$f" .mp4)"
        OUT="$OUTPUT_DIR/${BASE}_${det}.mp4"
        LOG="$DETAIL_DIR/${BASE}_${det}.log"

        SRC="${BASE%%__*}"
        OFFSET_CAT="${BASE#*__}"
        INJ=$(parse_injection "$BASE")

        # Run detector
        O=$("$AVSYNC" -i "$f" -o "$OUT" -m force -d "$det" -v -t "$THRESH" -w "$WINDOW" -s "$STEP" 2>&1) || true
        echo "$O" > "$LOG"

        if [ -z "$O" ]; then
            DET="TIMEOUT"
            CONF="0.00"
        else
            DET=$(parse_detection "$O")
            CONF=$(parse_avg_confidence "$O")
        fi

        EVAL=$(evaluate "$INJ" "$DET")
        RES=$(echo "$EVAL" | cut -d'|' -f1)
        EM=$(echo "$EVAL" | cut -d'|' -f2)
        NOTES=$(echo "$EVAL" | cut -d'|' -f3)

        # Update stats
        if [ "$det" = "onset_align" ]; then
            OA_TOTAL=$((OA_TOTAL+1))
            case "$RES" in PASS) OA_PASS=$((OA_PASS+1));; FAIL) OA_FAIL=$((OA_FAIL+1));; ERR) OA_ERR=$((OA_ERR+1));; esac
        else
            SN_TOTAL=$((SN_TOTAL+1))
            case "$RES" in PASS) SN_PASS=$((SN_PASS+1));; FAIL) SN_FAIL=$((SN_FAIL+1));; ERR) SN_ERR=$((SN_ERR+1));; esac
        fi

        echo "$RES" >> "$SRC_STATS_DIR/${SRC}_${det}.txt"

        printf "%-42s | %5sms | %5sms | %5sms | %5s | %6s | %s\n" "$BASE" "$INJ" "$DET" "$EM" "$CONF" "$RES" "$NOTES" | tee -a "$REPORT"
        echo "$SRC,$OFFSET_CAT,$INJ,$det,$DET,$EM,$CONF,$RES,$NOTES" >> "$CSV_REPORT"
    done
done

# ====================== SUMMARY ======================
{
echo ""
echo ""
echo "╔══════════════════════════════════════════════════════════════════════════╗"
echo "║                        SUMMARY COMPARISON                              ║"
echo "╚══════════════════════════════════════════════════════════════════════════╝"
echo ""
printf "%-15s | %6s | %6s | %6s | %6s | %9s\n" "Detector" "Total" "Pass" "Fail" "Error" "Pass Rate"
echo "------------------------------------------------------------------------"
if [ $OA_TOTAL -gt 0 ]; then
    OA_RATE=$(echo "scale=1; $OA_PASS * 100 / $OA_TOTAL" | bc)
    printf "%-15s | %6d | %6d | %6d | %6d | %8s%%\n" "onset_align" "$OA_TOTAL" "$OA_PASS" "$OA_FAIL" "$OA_ERR" "$OA_RATE"
fi
if [ $SN_TOTAL -gt 0 ]; then
    SN_RATE=$(echo "scale=1; $SN_PASS * 100 / $SN_TOTAL" | bc)
    printf "%-15s | %6d | %6d | %6d | %6d | %8s%%\n" "syncnet" "$SN_TOTAL" "$SN_PASS" "$SN_FAIL" "$SN_ERR" "$SN_RATE"
fi

echo ""
echo "--- Per-Source Breakdown ---"
echo ""
printf "%-12s | %22s | %22s\n" "Source" "onset_align" "syncnet"
echo "-------------------------------------------------------------------"

SOURCES=$(ls "$SRC_STATS_DIR"/*.txt 2>/dev/null | xargs -I{} basename {} | sed 's/_onset_align\.txt//;s/_syncnet\.txt//' | sort -u)
for src in $SOURCES; do
    printf "%-12s |" "$src"
    for det in "${DETECTORS[@]}"; do
        STAT_FILE="$SRC_STATS_DIR/${src}_${det}.txt"
        if [ -f "$STAT_FILE" ]; then
            T=$(wc -l < "$STAT_FILE" | tr -d ' ')
            P=$(grep -c "PASS" "$STAT_FILE" || true)
            RATE=$(echo "scale=1; $P * 100 / $T" | bc)
            printf " %3d/%3d (%5s%%) |" "$P" "$T" "$RATE"
        else
            printf " %22s |" "N/A"
        fi
    done
    echo ""
done

echo ""
echo "============================================================"
echo "Report: $REPORT"
echo "CSV:    $CSV_REPORT"
echo "Logs:   $DETAIL_DIR/"
echo "============================================================"
} | tee -a "$REPORT"

rm -rf "$SRC_STATS_DIR"
