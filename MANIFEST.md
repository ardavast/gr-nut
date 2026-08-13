title: The NUT OOT Module
brief: Generic ffmpeg-GNU-Radio interface (both directions) over a strict NUT profile
tags:
  - sdr
  - ffmpeg
  - nut
  - source
  - sink
author:
  - Ardavast Dayleryan <ardavast@noiseoverip.com>
copyright_owner:
  - Ardavast Dayleryan
license: GPL-3.0-or-later
gr_supported_version: 3.10
repo: https://github.com/ardavast/gr-nut
---
A source block that feeds GNU Radio flowgraphs from ffmpeg over a pipe,
and a sink block that feeds ffmpeg from flowgraphs (recording, encoding,
streaming), both using a strict NUT profile (pcm_f32le audio +
rawvideo/rgb24) as the interface. ffmpeg handles codecs, formats, and A/V
sync; GNU Radio is the physical layer and each chain has at most one
clock — ffmpeg is paced purely by pipe backpressure, so there is no
Throttle and no drift between streams.
