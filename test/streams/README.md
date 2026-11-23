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
bun gen_yuv.ts hbd16 in.yuv 96 64 9
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
bun rewrite_slices.ts base.bit SLICE_REFLIST_MOD_offsets.txt lists SLICE_REFLIST_MOD_HM18.bit
```

`in8.yuv` comes from `bun gen_yuv.ts main8 in8.yuv 128 96 9`.
`SLICE_REFLIST_MOD_offsets.txt` holds the bit offsets of the header fields the script edits (PPS
`lists_modification_present_flag`, each slice header's list-modification
position and end); they came from a decoder build with temporary prints in
the PPS and slice header parsers. The script sets the PPS flag, inserts the
list-modification syntax into every inter slice header, re-aligns the header
and drops the SEI. The pinned MD5 is HM's decoder output; ffmpeg gives the
same.

## MIXED_SLICES_HM18.bit

Pictures that mix an intra slice with an inter slice: from the third picture
on, one of the two slices is intra coded, alternately the second and the
first. The picture still has to keep its motion field and reference lists
for deblocking and for later pictures. HM never encodes this, so slices of an
all-intra encode are spliced into the inter stream above (`base.bit`):

```
TAppEncoder -c cfg/encoder_intra_main.cfg <same options as base.bit> -b intra.bit
bun splice_intra_slices.ts base.bit MIXED_SLICES_inter_offsets.txt \
  intra.bit MIXED_SLICES_intra_offsets.txt MIXED_SLICES_HM18.bit
```

Both encodes produce the same PPS, which the splice relies on. The offsets
files again come from temporary prints in the slice header parser (positions
of slice_type, the SAO flags, the P/B-only block, slice_qp_delta and the
header end). The pinned MD5 is HM's decoder output; ffmpeg gives the same.
A decoder build from before the mixed-slice fix gets 7 of the 9 frames wrong.

`annexb.ts` holds the bitstream helpers both scripts share.

The `rewrite_slices.ts` script's `pps` mode (second slice names a second PPS) produces an invalid
stream: H.265 7.4.7.1 requires the same PPS in all slices of a picture.
