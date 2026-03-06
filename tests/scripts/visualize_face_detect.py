#!/usr/bin/env python3
"""Visualize YuNet face detection results on video frames.

Draws face bounding boxes and lip regions on sampled video frames,
saving annotated images for debugging the SyncNet detector.
"""

import argparse
import sys
from pathlib import Path

try:
    import cv2
    import numpy as np
except ImportError:
    print("ERROR: OpenCV is required. Install with: pip3 install opencv-python", file=sys.stderr)
    sys.exit(1)


def find_model(explicit_path: str = None) -> str:
    """Locate the YuNet face detection model."""
    if explicit_path and Path(explicit_path).is_file():
        return explicit_path

    script_dir = Path(__file__).resolve().parent
    project_dir = script_dir.parent.parent
    candidates = [
        project_dir / "build" / "bin" / "models" / "face_detection_yunet_2023mar.onnx",
        project_dir / "models" / "face_detection_yunet_2023mar.onnx",
        project_dir / "cmake-build-debug" / "bin" / "models" / "face_detection_yunet_2023mar.onnx",
    ]
    for c in candidates:
        if c.is_file():
            return str(c)

    print("ERROR: Cannot find YuNet model. Use --model <path>.", file=sys.stderr)
    sys.exit(1)


def extract_lip_region(face_x, face_y, face_w, face_h, ratio=0.35):
    """Extract lip region from face bounding box (matches C++ logic)."""
    lip_y = face_y + int(face_h * (1.0 - ratio))
    lip_height = int(face_h * ratio)
    lip_x = face_x + face_w // 6
    lip_width = face_w * 2 // 3
    return lip_x, lip_y, lip_width, lip_height


def main():
    parser = argparse.ArgumentParser(description="Visualize face detection on video frames")
    parser.add_argument("-i", "--input", required=True, help="Input video file")
    parser.add_argument("-o", "--output", default="face_detect_vis", help="Output directory for annotated frames")
    parser.add_argument("--model", default=None, help="Path to YuNet ONNX model")
    parser.add_argument("--num-frames", type=int, default=20, help="Number of frames to sample (default: 20)")
    parser.add_argument("--score-threshold", type=float, default=0.7, help="Face detection score threshold")
    parser.add_argument("--min-face-size", type=int, default=60, help="Minimum face size in pixels")
    parser.add_argument("--montage", action="store_true", help="Create a montage of all sampled frames")
    args = parser.parse_args()

    video_path = Path(args.input)
    if not video_path.is_file():
        print(f"ERROR: Video not found: {args.input}", file=sys.stderr)
        sys.exit(1)

    model_path = find_model(args.model)
    print(f"Model: {model_path}")

    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)

    # Open video
    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        print(f"ERROR: Cannot open video: {args.input}", file=sys.stderr)
        sys.exit(1)

    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    fps = cap.get(cv2.CAP_PROP_FPS)
    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    duration = total_frames / fps if fps > 0 else 0

    print(f"Video: {video_path.name}")
    print(f"  Resolution: {width}x{height}")
    print(f"  FPS: {fps:.2f}")
    print(f"  Duration: {duration:.2f}s ({total_frames} frames)")
    print(f"  Sampling {args.num_frames} frames")
    print()

    # Create face detector
    detector = cv2.FaceDetectorYN.create(
        model_path,
        "",
        (width, height),  # use actual video resolution
        args.score_threshold,
        0.3,   # NMS threshold
        5000   # top-K
    )

    # Sample frames evenly across the video
    sample_indices = np.linspace(0, total_frames - 1, args.num_frames, dtype=int)

    annotated_frames = []
    face_stats = {"total_frames": 0, "frames_with_face": 0, "total_faces": 0}
    all_face_positions = []

    for frame_idx in sample_indices:
        cap.set(cv2.CAP_PROP_POS_FRAMES, frame_idx)
        ret, frame = cap.read()
        if not ret:
            continue

        face_stats["total_frames"] += 1
        timestamp = frame_idx / fps if fps > 0 else 0

        # Detect faces
        _, faces_mat = detector.detect(frame)

        n_faces = 0
        frame_faces = []

        if faces_mat is not None:
            for i in range(faces_mat.shape[0]):
                x = int(faces_mat[i, 0])
                y = int(faces_mat[i, 1])
                w = int(faces_mat[i, 2])
                h = int(faces_mat[i, 3])
                score = faces_mat[i, 14]

                # Filter by min face size (matching C++ logic)
                if w < args.min_face_size or h < args.min_face_size:
                    continue

                n_faces += 1
                frame_faces.append((x, y, w, h, score))
                all_face_positions.append({
                    "frame": int(frame_idx),
                    "time": timestamp,
                    "x": x, "y": y, "w": w, "h": h,
                    "score": score
                })

                # Draw face bounding box (green)
                cv2.rectangle(frame, (x, y), (x + w, y + h), (0, 255, 0), 2)

                # Draw face score
                label = f"Face {i}: {score:.2f}"
                cv2.putText(frame, label, (x, y - 10),
                           cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2)

                # Draw lip region (red)
                lx, ly, lw, lh = extract_lip_region(x, y, w, h)
                cv2.rectangle(frame, (lx, ly), (lx + lw, ly + lh), (0, 0, 255), 2)
                cv2.putText(frame, "lip", (lx, ly - 5),
                           cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 0, 255), 1)

                # Draw 5 facial landmarks (if available)
                for j in range(5):
                    lm_x = int(faces_mat[i, 4 + j * 2])
                    lm_y = int(faces_mat[i, 5 + j * 2])
                    cv2.circle(frame, (lm_x, lm_y), 3, (255, 0, 0), -1)

        if n_faces > 0:
            face_stats["frames_with_face"] += 1
            face_stats["total_faces"] += n_faces

        # Draw frame info
        info = f"Frame {frame_idx} | t={timestamp:.2f}s | Faces: {n_faces}"
        cv2.putText(frame, info, (10, 30),
                   cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 255, 0), 2)

        # Status indicator
        status = "FACE DETECTED" if n_faces > 0 else "NO FACE"
        color = (0, 255, 0) if n_faces > 0 else (0, 0, 255)
        cv2.putText(frame, status, (10, 60),
                   cv2.FONT_HERSHEY_SIMPLEX, 0.7, color, 2)

        # Save individual frame
        out_path = output_dir / f"frame_{frame_idx:06d}_t{timestamp:.1f}s.jpg"
        cv2.imwrite(str(out_path), frame)

        face_info = ", ".join([f"({f[0]},{f[1]},{f[2]}x{f[3]}) score={f[4]:.2f}" for f in frame_faces])
        print(f"  Frame {frame_idx:5d} | t={timestamp:6.2f}s | Faces: {n_faces} | {face_info}")

        annotated_frames.append(frame)

    cap.release()

    # Print summary
    print()
    print("=" * 60)
    print("SUMMARY")
    print("=" * 60)
    print(f"  Frames sampled: {face_stats['total_frames']}")
    print(f"  Frames with face: {face_stats['frames_with_face']}")
    print(f"  Total faces detected: {face_stats['total_faces']}")
    det_rate = face_stats['frames_with_face'] / max(1, face_stats['total_frames']) * 100
    print(f"  Detection rate: {det_rate:.1f}%")

    if all_face_positions:
        xs = [f["x"] for f in all_face_positions]
        ys = [f["y"] for f in all_face_positions]
        ws = [f["w"] for f in all_face_positions]
        hs = [f["h"] for f in all_face_positions]
        scores = [f["score"] for f in all_face_positions]

        print()
        print("  Face position statistics:")
        print(f"    X range: [{min(xs)}, {max(xs)}]  (spread: {max(xs)-min(xs)})")
        print(f"    Y range: [{min(ys)}, {max(ys)}]  (spread: {max(ys)-min(ys)})")
        print(f"    W range: [{min(ws)}, {max(ws)}]  (spread: {max(ws)-min(ws)})")
        print(f"    H range: [{min(hs)}, {max(hs)}]  (spread: {max(hs)-min(hs)})")
        print(f"    Score range: [{min(scores):.2f}, {max(scores):.2f}]")

        # Check if face position is stable
        x_spread = max(xs) - min(xs)
        y_spread = max(ys) - min(ys)
        if x_spread > width * 0.3 or y_spread > height * 0.3:
            print()
            print("  ⚠️  WARNING: Face positions vary widely across frames!")
            print("     This suggests multiple different faces or camera changes.")
            print("     SyncNet lip-motion tracking may be unreliable.")
        else:
            print()
            print("  ✅ Face positions are relatively stable across frames.")

    # Create montage if requested
    if args.montage and annotated_frames:
        cols = min(5, len(annotated_frames))
        rows = (len(annotated_frames) + cols - 1) // cols

        # Resize frames for montage
        thumb_w, thumb_h = 384, 216
        thumbnails = []
        for f in annotated_frames:
            thumb = cv2.resize(f, (thumb_w, thumb_h))
            thumbnails.append(thumb)

        # Pad to fill grid
        while len(thumbnails) < rows * cols:
            thumbnails.append(np.zeros((thumb_h, thumb_w, 3), dtype=np.uint8))

        montage_rows = []
        for r in range(rows):
            row_imgs = thumbnails[r * cols:(r + 1) * cols]
            montage_rows.append(np.hstack(row_imgs))
        montage = np.vstack(montage_rows)

        montage_path = output_dir / "montage.jpg"
        cv2.imwrite(str(montage_path), montage)
        print(f"\n  Montage saved: {montage_path}")

    print(f"\n  Output directory: {output_dir}")


if __name__ == "__main__":
    main()
