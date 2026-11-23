# Checked-in HEVC test streams

Raw Annex B streams that no public conformance set provides. `bun cmd/tests.ts
-all` decodes them with `heic_test -hevc-sequence` and compares the output MD5
with the one pinned in `HEVC_SEQUENCE_TESTS` (`cmd/tests.ts`).

## HBD16_INTER_*_HM18.bit

16-bit 4:4:4 with inter prediction. The JCT-VC range-extensions streams above
12 bits are intra-only or too dark to matter, so nothing else takes the
prediction intermediates past 16 bits (a 16-bit sample `<< 2` needs 18).

Made with HM 18.0 built with `-DHIGH_BITDEPTH=ON`; the pinned MD5 is the
output of that build's `TAppDecoder` (which also verified the picture hash
SEI of every frame):

```
python gen_hbd_yuv.py in.yuv 96 64 9
TAppEncoder -c cfg/encoder_lowdelay_main.cfg -i in.yuv -wdt 96 -hgt 64 -fr 30 -f 9 \
  --InputBitDepth=16 --InternalBitDepth=16 --InputChromaFormat=444 \
  --Profile=main-RExt -q 12 --SEIDecodedPictureHash=1 \
  --ExtendedPrecision=0 --WeightedPredP=0 --WeightedPredB=0 -b HBD16_INTER_HM18.bit
# HBD16_INTER_EXTPREC_WP_HM18.bit: the same with the last three options set to 1
TAppDecoder -b <stream> -o out.yuv -d 0
```

Each stream is one I picture followed by eight B pictures.
