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

## SLICE_REFLIST_MOD_HM18.bit

8-bit 4:2:0, 128x96, two slices per picture, where the second slice of each
picture reverses reference list L0 with `ref_pic_list_modification`. The
slices of one picture then use different reference lists, which matters for
deblocking across the slice edge and for collocated motion vectors. HM never
encodes this, so an HM stream is rewritten:

```
TAppEncoder -c cfg/encoder_lowdelay_main.cfg -i in8.yuv -wdt 128 -hgt 96 -fr 30 -f 9 \
  --InputBitDepth=8 -q 30 --MaxCUWidth=16 --MaxCUHeight=16 --MaxPartitionDepth=2 \
  --QuadtreeTULog2MaxSize=4 --SliceMode=1 --SliceArgument=24 \
  --LFCrossSliceBoundaryFlag=1 --SEIDecodedPictureHash=0 -b base.bit
python rewrite_slices.py base.bit SLICE_REFLIST_MOD_offsets.txt lists SLICE_REFLIST_MOD_HM18.bit
```

`in8.yuv` is the pattern of `gen_hbd_yuv.py` at 8 bits, 4:2:0.
`SLICE_REFLIST_MOD_offsets.txt` holds the bit offsets of the header fields the script edits (PPS
`lists_modification_present_flag`, each slice header's list-modification
position and end); they came from a decoder build with temporary prints in
the PPS and slice header parsers. The script sets the PPS flag, inserts the
list-modification syntax into every inter slice header, re-aligns the header
and drops the SEI. The pinned MD5 is HM's decoder output; ffmpeg gives the
same.

The script's `pps` mode (second slice names a second PPS) produces an invalid
stream: H.265 7.4.7.1 requires the same PPS in all slices of a picture.
