"""Make the frame file of tekken_wmv3_bench (app/src/cpu/tekken_wmv3_bench.cpp) from one of the game's movies.

    python tools/wmv3_bench_extract.py <game>/DATA320.BIN out/wmv3_bench.bin [--frames 900]

The movies are ASF files (WMV3 video). The sequence header (extradata) is read from the ASF video stream's
BITMAPINFOHEADER; the frames come from ffmpeg (-c copy -f data) and their sizes / key flags from ffprobe, both in the
same order. Needs ffmpeg and ffprobe on PATH.
"""
import argparse
import struct
import subprocess
import sys

FILE_PROPS = bytes.fromhex("A1DCAB8C47A9CF118EE400C00C205365")
STREAM_PROPS = bytes.fromhex("9107DCB7B7A9CF118EE600C00C205365")
VIDEO_MEDIA = bytes.fromhex("C0EF19BC4D5BCF11A8FD00805F5C442B")


def asf_video_header(path):
    head = open(path, "rb").read(1 << 20)
    j = 0
    while True:
        j = head.find(STREAM_PROPS, j + 1)
        if j < 0:
            sys.exit("no ASF video stream in " + path)
        if head[j + 24:j + 40] == VIDEO_MEDIA:
            break
    tsd = j + 24 + 16 + 16 + 8 + 4 + 4 + 2 + 4  # type-specific data
    width, height = struct.unpack_from("<II", head, tsd)
    bih = tsd + 11
    bi_size = struct.unpack_from("<I", head, bih)[0]
    fourcc = head[bih + 16:bih + 20]
    if fourcc != b"WMV3":
        sys.exit("video is %r, not WMV3" % fourcc)
    return width, height, head[bih + 40:bih + bi_size]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("movie")
    ap.add_argument("out")
    ap.add_argument("--frames", type=int, default=900)
    a = ap.parse_args()

    width, height, extra = asf_video_header(a.movie)
    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "packet=size,flags",
                            "-of", "csv=p=0", a.movie], capture_output=True, text=True, check=True).stdout.split()
    packets = []
    for row in probe:
        size, flags = row.split(",")[:2]
        packets.append((int(size), 1 if flags.startswith("K") else 0))
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", a.movie, "-map", "0:v:0", "-c", "copy", "-f", "data", "-"],
                         capture_output=True, check=True).stdout
    if sum(s for s, _ in packets) != len(raw):
        sys.exit("ffprobe sizes (%d) and ffmpeg bytes (%d) differ" % (sum(s for s, _ in packets), len(raw)))

    packets = packets[:a.frames]
    with open(a.out, "wb") as f:
        f.write(b"WMV3BNCH" + struct.pack("<III", width, height, len(extra)) + extra + struct.pack("<I", len(packets)))
        at = 0
        for size, key in packets:
            f.write(struct.pack("<II", size, key) + raw[at:at + size])
            at += size
    print("%dx%d, extradata %s, %d frames (%d key), %.1f MB -> %s" % (
        width, height, extra.hex(), len(packets), sum(k for _, k in packets), at / 1048576, a.out))


if __name__ == "__main__":
    main()
