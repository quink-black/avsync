#!/bin/bash
# Generate test samples with known AV offsets using FFmpeg
# Usage: ./generate_test_samples.sh <source_video> <output_dir>

set -euo pipefail

SOURCE="${1:?Usage: $0 <source_video> <output_dir>}"
OUTPUT_DIR="${2:?Usage: $0 <source_video> <output_dir>}"

mkdir -p "$OUTPUT_DIR"

echo "=== Generating AV sync test samples ==="
echo "Source: $SOURCE"
echo "Output: $OUTPUT_DIR"

# Test 1: Audio delayed by 100ms (video ahead)
echo "[1/6] Audio delayed by 100ms..."
ffmpeg -y -i "$SOURCE" \
    -itsoffset 0.1 -i "$SOURCE" \
    -map 0:v -map 1:a \
    -c copy \
    "$OUTPUT_DIR/offset_audio_delay_100ms.mp4" \
    2>/dev/null

# Test 2: Audio ahead by 200ms
echo "[2/6] Audio ahead by 200ms..."
ffmpeg -y -i "$SOURCE" \
    -itsoffset -0.2 -i "$SOURCE" \
    -map 0:v -map 1:a \
    -c copy \
    "$OUTPUT_DIR/offset_audio_ahead_200ms.mp4" \
    2>/dev/null

# Test 3: Audio delayed by 50ms (near threshold)
echo "[3/6] Audio delayed by 50ms (near threshold)..."
ffmpeg -y -i "$SOURCE" \
    -itsoffset 0.05 -i "$SOURCE" \
    -map 0:v -map 1:a \
    -c copy \
    "$OUTPUT_DIR/offset_audio_delay_50ms.mp4" \
    2>/dev/null

# Test 4: Audio delayed by 30ms (below threshold, should be skipped)
echo "[4/6] Audio delayed by 30ms (below threshold)..."
ffmpeg -y -i "$SOURCE" \
    -itsoffset 0.03 -i "$SOURCE" \
    -map 0:v -map 1:a \
    -c copy \
    "$OUTPUT_DIR/offset_audio_delay_30ms.mp4" \
    2>/dev/null

# Test 5: No offset (reference / baseline)
echo "[5/6] No offset (baseline)..."
ffmpeg -y -i "$SOURCE" \
    -c copy \
    "$OUTPUT_DIR/no_offset_baseline.mp4" \
    2>/dev/null

# Test 6: Large offset (500ms audio delay)
echo "[6/6] Audio delayed by 500ms..."
ffmpeg -y -i "$SOURCE" \
    -itsoffset 0.5 -i "$SOURCE" \
    -map 0:v -map 1:a \
    -c copy \
    "$OUTPUT_DIR/offset_audio_delay_500ms.mp4" \
    2>/dev/null

echo ""
echo "=== Generated test samples ==="
echo "Expected offsets:"
echo "  offset_audio_delay_100ms.mp4  -> audio 100ms behind video"
echo "  offset_audio_ahead_200ms.mp4  -> audio 200ms ahead of video"
echo "  offset_audio_delay_50ms.mp4   -> audio 50ms behind video (near threshold)"
echo "  offset_audio_delay_30ms.mp4   -> audio 30ms behind video (below 40ms threshold, should skip)"
echo "  no_offset_baseline.mp4        -> no offset (baseline)"
echo "  offset_audio_delay_500ms.mp4  -> audio 500ms behind video (large offset)"
echo ""
echo "Test with: avsync -i <test_file> -o <output_file> -v"
