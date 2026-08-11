title: The NUT OOT Module
brief: Generic ffmpeg-to-GNU-Radio ingest over a strict NUT profile
tags:
  - sdr
  - ffmpeg
  - nut
  - source
author:
  - Ardavast Dayleryan <ardavast@noiseoverip.com>
copyright_owner:
  - Ardavast Dayleryan
license: GPL-3.0-or-later
gr_supported_version: 3.10
repo: https://github.com/ardavast/gr-nut
---
A source block that feeds GNU Radio flowgraphs from ffmpeg over a pipe,
using a strict NUT profile (pcm_f32le audio + rawvideo/rgb24) as the
interface. ffmpeg handles codecs, formats, and A/V sync; GNU Radio is the
physical layer and the SDR sink is the only clock — ffmpeg is paced purely
by pipe backpressure, so there is no Throttle and no drift between streams.
