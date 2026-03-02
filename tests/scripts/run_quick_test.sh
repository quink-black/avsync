#!/bin/bash
# =============================================================================
# AV Auto-Sync: Quick Regression Test (kuaishou + pig priority)
# =============================================================================
set -uo pipefail

# Derive paths relative to this script's location
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Allow overriding via environment variables
AVSYNC="${AVSYNC:-$PROJECT_DIR/build/bin/avsync}"
INPUT_DIR="${AVSYNC_INPUT_DIR:-}"
OUTPUT_DIR="${AVSYNC_OUTPUT_DIR:-}"
REPORT_DIR="${AVSYNC_REPORT_DIR:-}"

# If INPUT_DIR not set, check for test_samples in common locations
if [ -z "$INPUT_DIR" ]; then
    if [ -d "$PROJECT_DIR/test_samples" ]; then
        INPUT_DIR="$PROJECT_DIR/test_samples"
    elif [ -f "$PROJECT_DIR/tests/test_config.json" ]; then
        # Try to read from test_config.json
        CONFIGURED_DIR=$(python3 -c "import json; print(json.load(open('$PROJECT_DIR/tests/test_config.json')).get('test_samples_dir',''))" 2>/dev/null || true)
        if [ -n "$CONFIGURED_DIR" ] && [ -d "$CONFIGURED_DIR" ]; then
            INPUT_DIR="$CONFIGURED_DIR"
        fi
    fi
fi

if [ -z "$INPUT_DIR" ] || [ ! -d "$INPUT_DIR" ]; then
    echo "ERROR: Test samples directory not found."
    echo "  Set AVSYNC_INPUT_DIR environment variable or create test_samples/ in project root."
    echo "  Usage: AVSYNC_INPUT_DIR=/path/to/samples $0"
    exit 1
fi

# Default output and report dirs alongside the input
PARENT_OF_INPUT="$(dirname "$INPUT_DIR")"
OUTPUT_DIR="${OUTPUT_DIR:-$PARENT_OF_INPUT/test_output}"
REPORT_DIR="${REPORT_DIR:-$PARENT_OF_INPUT/test_reports}"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
REPORT="$REPORT_DIR/report_${TIMESTAMP}.txt"
DETAIL_DIR="$REPORT_DIR/details_${TIMESTAMP}"
mkdir -p "$OUTPUT_DIR" "$REPORT_DIR" "$DETAIL_DIR"
TOLERANCE=40; THRESH=40; TOTAL=0; PASS=0; FAIL=0; ERR=0

# Use larger segment window for more reliable detection
WINDOW=10.0
STEP=5.0

{
echo "============================================================"
echo " AV Auto-Sync Regression Test Report"
echo " $(date)"
echo " Tolerance: ±${TOLERANCE}ms | Threshold: ${THRESH}ms"
echo " Segment window: ${WINDOW}s | Step: ${STEP}s"
echo " Input dir: ${INPUT_DIR}"
echo "============================================================"
echo ""
printf "%-50s | %7s | %7s | %8s | %6s | %s\n" "File" "Inject" "Detect" "Error" "Result" "Notes"
echo "---------------------------------------------------------------------------------------------------------------------------"
} | tee "$REPORT"

# Find all test sample mp4 files, prioritize kuaishou and pig
for f in "$INPUT_DIR"/*__*.mp4; do
    [ -f "$f" ] || continue
    BASE="$(basename "$f" .mp4)"
    OUT="$OUTPUT_DIR/${BASE}_c.mp4"
    LOG="$DETAIL_DIR/${BASE}.log"
    TOTAL=$((TOTAL+1))

    # Parse injected offset from filename pattern: *_pos_NNms or *_neg_NNms or *_0ms
    case "$BASE" in
        *pos_20ms) INJ=20;;  *neg_30ms) INJ=-30;; *pos_40ms) INJ=40;;
        *pos_50ms) INJ=50;;  *neg_45ms) INJ=-45;; *pos_80ms) INJ=80;;
        *pos_100ms) INJ=100;; *neg_100ms) INJ=-100;; *pos_150ms) INJ=150;;
        *pos_200ms) INJ=200;; *neg_250ms) INJ=-250;; *pos_500ms) INJ=500;;
        *pos_1000ms) INJ=1000;; *neg_1000ms) INJ=-1000;;
        *pos_2000ms) INJ=2000;; *neg_2000ms) INJ=-2000;;
        *0ms) INJ=0;; *) INJ=0;;
    esac
    O=$("$AVSYNC" -i "$f" -o "$OUT" -v -t "$THRESH" -w "$WINDOW" -s "$STEP" 2>&1) || true
    echo "$O" > "$LOG"

    # Parse detected average correction from report
    DET=""
    OFS=$(echo "$O" | grep -A200 "Correction Decisions" | grep "YES" | awk '{print $3}')
    if [ -n "$OFS" ]; then
        S=0; C=0
        for v in $OFS; do S=$(echo "$S + $v" | bc); C=$((C+1)); done
        [ $C -gt 0 ] && DET=$(echo "scale=1; $S / $C" | bc)
    fi
    if [ -z "$DET" ]; then
        if echo "$O" | grep -q "no corrections\|0 segments corrected"; then
            DET="0"
        else
            DET="N/A"
        fi
    fi

    RES="ERR"; N=""; EM="N/A"
    AI=$(echo "${INJ#-}" | bc)
    AD=$(echo "${DET#-}" | bc 2>/dev/null || echo 0)
    if [ "$DET" = "N/A" ]; then
        RES="ERR"; ERR=$((ERR+1)); N="no output"
    elif [ "$INJ" = "0" ]; then
        if (( $(echo "$AD < $TOLERANCE" | bc -l) )); then
            RES="PASS"; PASS=$((PASS+1)); N="baseline ok"
        else
            RES="FAIL"; FAIL=$((FAIL+1)); N="false positive"
        fi
    elif (( $(echo "$AI < $THRESH" | bc -l) )); then
        if (( $(echo "$AD < $THRESH" | bc -l) )); then
            RES="PASS"; PASS=$((PASS+1)); N="below thresh, skipped ok"
        else
            RES="FAIL"; FAIL=$((FAIL+1)); N="below thresh but corrected"
        fi
    elif [ "$DET" != "0" ]; then
        EM=$(echo "scale=1; $DET - ($INJ)" | bc)
        AE=$(echo "${EM#-}" | bc)
        if (( $(echo "$AE <= $TOLERANCE" | bc -l) )); then
            RES="PASS"; PASS=$((PASS+1)); N="err:${EM}ms"
        else
            RES="FAIL"; FAIL=$((FAIL+1)); N="err:${EM}ms (>±40ms)"
        fi
    else
        RES="FAIL"; FAIL=$((FAIL+1)); N="should correct but missed"
    fi
    printf "%-50s | %5sms | %5sms | %8s | %6s | %s\n" "$BASE" "$INJ" "$DET" "$EM" "$RES" "$N" | tee -a "$REPORT"
done

{
echo ""
echo "============================================================"
echo "SUMMARY"
echo "  Total:  $TOTAL"
echo "  PASS:   $PASS ($(echo "scale=1;$PASS*100/$TOTAL" | bc)%)"
echo "  FAIL:   $FAIL ($(echo "scale=1;$FAIL*100/$TOTAL" | bc)%)"
echo "  ERROR:  $ERR"
echo "============================================================"
echo "Report: $REPORT"
echo "Details: $DETAIL_DIR/"
} | tee -a "$REPORT"
