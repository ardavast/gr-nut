#!/usr/bin/env python3
# -*- coding: utf-8 -*-

#
# SPDX-License-Identifier: GPL-3.0
#
# GNU Radio Python Flow Graph
# Title: gr-nut M3 video-to-file dump
# Author: gr-nut
# Description: M3 video path validation: NUT video ingest -> raw rgb24 frame dump to a file. No modulator, no display; use video_check.sh to verify the dump byte-exactly against ffmpeg.
# GNU Radio version: 3.10.9.2

from gnuradio import blocks
from gnuradio import gr
from gnuradio.filter import firdes
from gnuradio.fft import window
import sys
import signal
from argparse import ArgumentParser
from gnuradio.eng_arg import eng_float, intx
from gnuradio import eng_notation
from gnuradio import nut




class video_to_file(gr.top_block):

    def __init__(self, uri='/tmp/video.nut', outfile='frames.rgb'):
        gr.top_block.__init__(self, "gr-nut M3 video-to-file dump", catch_exceptions=True)

        ##################################################
        # Parameters
        ##################################################
        self.uri = uri
        self.outfile = outfile

        ##################################################
        # Blocks
        ##################################################

        self.nut_nut_source_video_0 = nut.nut_source(uri, 0, 1, '')
        self.blocks_file_sink_0 = blocks.file_sink(gr.sizeof_char*1, outfile, False)
        self.blocks_file_sink_0.set_unbuffered(False)


        ##################################################
        # Connections
        ##################################################
        self.connect((self.nut_nut_source_video_0, 0), (self.blocks_file_sink_0, 0))


    def get_uri(self):
        return self.uri

    def set_uri(self, uri):
        self.uri = uri

    def get_outfile(self):
        return self.outfile

    def set_outfile(self, outfile):
        self.outfile = outfile
        self.blocks_file_sink_0.open(self.outfile)



def argument_parser():
    description = 'M3 video path validation: NUT video ingest -> raw rgb24 frame dump to a file. No modulator, no display; use video_check.sh to verify the dump byte-exactly against ffmpeg.'
    parser = ArgumentParser(description=description)
    parser.add_argument(
        "--uri", dest="uri", type=str, default='/tmp/video.nut',
        help="Set NUT FIFO/file [default=%(default)r]")
    parser.add_argument(
        "--outfile", dest="outfile", type=str, default='frames.rgb',
        help="Set Output raw rgb24 file [default=%(default)r]")
    return parser


def main(top_block_cls=video_to_file, options=None):
    if options is None:
        options = argument_parser().parse_args()
    tb = top_block_cls(uri=options.uri, outfile=options.outfile)

    def sig_handler(sig=None, frame=None):
        tb.stop()
        tb.wait()

        sys.exit(0)

    signal.signal(signal.SIGINT, sig_handler)
    signal.signal(signal.SIGTERM, sig_handler)

    tb.start()

    tb.wait()


if __name__ == '__main__':
    main()
