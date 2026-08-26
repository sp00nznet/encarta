# encextract — end-to-end Encarta 97 content pipeline

One command turns a mounted Encarta 97 data disc into a browsable content tree,
using only the project's recompiled / clean-room tools — **no original Encarta
code runs in the decode path**.

```
m20dump (-x raw) ──▶ recomp_decode (statically-recompiled FTC codec) ──▶ PNG
                 └─▶ mvbtext (MVB 2.0 titles + literal text)          ──▶ titles.txt, HTML
```

## Use

```bash
# build the native tools once
cmake --build build --config Release --target m20dump recomp_decode

# mount a disc (PowerShell: Mount-DiskImage CD1ENC97ENC.iso), then:
py tools/encextract/encextract.py <drive>:\ENCYC97 out_dir --max-images 200
# -> out_dir/titles.txt   (~31,517 article titles)
#    out_dir/images/*.png  (FTC photos decoded by the recompiled codec; .RLE->.bmp)
#    out_dir/index.html    (gallery)
```

## What it does

1. **Titles** — extracts all article titles from `ENCARTA.M20` (`_TTLBTREE`).
2. **Images** — extracts `PICON.M20` (216×192 thumbnails) and `MAXMED1.M20`
   (full-size photos, up to 512×344) and decodes the FTC images through the
   **statically-recompiled DECO_32 codec** (`recomp_decode`); inline `.RLE`
   entries are plain BMPs, copied as-is.
3. **Index** — writes a simple `index.html` gallery.

`--max-images` caps total decoded images (shared across sources) for quick runs.

## Status / notes

- Decodes Encarta's **entire image library** (thumbnails + full-size photos +
  inline graphics) — see [`../recomp`](../recomp) for the codec recompilation.
- Article **titles** are complete, and so is the prose: the phrase encoding was
  decoded, so `mvbtext prose <entry>` reads real article text rather than
  fragments (see [`../mvbtext`](../mvbtext)). This pipeline still writes only
  titles - wiring the prose into the HTML is not done.
- Output is Microsoft's content — keep it local; do not redistribute.
