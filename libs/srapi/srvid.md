# srvid — Simple Raw Video format

`srvid` (Simple Raw VIDeo) is a thin container OneTool uses to capture frames
straight from a DRM scanout buffer to disk. It is **not** a polished video
codec: the goal is to make the recording path inside `srapi_drm_record_*`
cheap enough that a compositor or demo can keep rendering at full speed while
the OS dumps frames in the background.

The captured `.srvid` files are then converted to a real container with
`tools/dev/converter/srvid2mp4` (see that tool's `--help`).

## On-disk layout

The file is little-endian and starts with a fixed 32-byte header followed by
a stream of zstd-compressed frames.

```
offset  size  field
------  ----  -----
0       4     magic        ASCII "SRVD"
4       4     version      uint32  (current: 1)
8       4     width        uint32  pixels
12      4     height       uint32  pixels
16      4     fps_millihz  uint32  frames-per-second × 1000 (e.g. 60000 = 60 fps)
20      4     pixfmt       uint32  0 = BGRA8888 (only format defined today)
24      8     reserved     8 zero bytes (future expansion)
32      ...   frames
```

After the header the body is a sequence of frame records:

```
4 bytes   csize       uint32 little-endian, length of the next compressed block
csize     payload     raw zstd frame (uncompressed size = width*height*4)
```

Decompressing each payload yields exactly `width*height*4` bytes of BGRA8888
(matching the layout of the DRM front buffer).

End-of-stream is indicated by EOF — there is no terminator record. Readers
that hit a partial trailing record should treat the recording as truncated
but otherwise valid.

## Producer pipeline (libs/srapi/src/backend/drm/record.c)

`srapi_drm_record_start(display, path, fps_millihz)` spins up:

* Three aligned 64-byte slot buffers that hold raw frame copies.
* A single zstd encoder context (level 1 by default; multi-worker if the
  installed zstd supports it).
* A worker pthread that compresses + writes frames in the background.
* A megabyte-sized stdio output buffer (`setvbuf`) so the kernel sees large
  sequential writes instead of one syscall per frame.

`srapi_drm_record_capture()` is invoked once per swap-buffer in the main
DRM presentation loop. It:

1. Throttles to the requested fps using `CLOCK_MONOTONIC` (`interval_ns =
   1e12 / fps_millihz`).
2. Copies the current front buffer into a free slot using a non-temporal
   `MOVNTDQA`-based SSE4 path on x86, falling back to plain `memcpy`.
3. Signals the worker.
4. If all slots are full the frame is **dropped** (counted in `r->dropped`).

`srapi_drm_record_stop(display)` flushes the queue and closes the file.

This design intentionally keeps the hot path tiny — the renderer never blocks
on compression — at the cost of occasionally dropping frames when zstd can't
keep up.

## Pixel format

Currently only `pixfmt = 0 = BGRA8888` is emitted. On little-endian hosts the
in-memory layout of each pixel matches what `srapi_framebuffer_pixels()`
returns (low byte = B, high byte = A). When piping to ffmpeg the matching
`-pix_fmt bgra` is used.

## Sizing notes

* Raw frame: `width * height * 4` bytes (≈8 MB at 1920×1080).
* Compressed frame: usually 5–20% of raw on UI/desktop content, since zstd
  at level 1 captures the long flat-color spans typical of compositors well.
* The on-disk frame count is bounded only by free disk space + fps.

## Producers in the tree

* `srapi_drm_record_start` — primitive API, see header for the full signature.
* `tools/dev/srapi_demo --record file.srvid [--record-fps n]`
* `tools/games/ranal_demo --record file.srvid [--record-fps n]`
* `libs/gui/swm` — pass `--record file.srvid [--record-fps n]` to capture
  the whole compositor (including the DE taskbar and every client surface).
* Any user of ranal can call `ranal_record_start(path, fps)` — that is a thin
  wrapper around `srapi_drm_record_start`.

## Consumers

* `tools/dev/converter/srvid2mp4 in.srvid out.mp4 [--codec libx264] [--crf N]`
  pipes decoded frames into `ffmpeg -f rawvideo -pix_fmt bgra ...`.
