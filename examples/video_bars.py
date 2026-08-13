#!/usr/bin/env python3
# -*- coding: utf-8 -*-

#
# SPDX-License-Identifier: GPL-3.0
#
# GNU Radio Python Flow Graph
# Title: gr-nut video bars example
# Author: gr-nut
# Description: nut_sink video example: a vector source holding one frame of eight vertical color bars (white/yellow/cyan/green/magenta/red/blue/black, rgb24, row-major) repeats it into the NUT Video Sink, which cuts the byte stream into frames, stamps pts from the frame count and feeds a spawned ffmpeg encoding H.264 to /tmp/bars.mkv. A K0 chain: no pacer anywhere, the graph free-runs and renders `seconds` seconds of video in a fraction of that. Play the result with any video player.
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




class video_bars(gr.top_block):

    def __init__(self, command='ffmpeg -y -loglevel warning -i pipe:0 -c:v libx264 -preset veryfast -pix_fmt yuv420p /tmp/bars.mkv', uri='', seconds=5):
        gr.top_block.__init__(self, "gr-nut video bars example", catch_exceptions=True)

        ##################################################
        # Parameters
        ##################################################
        self.command = command
        self.uri = uri
        self.seconds = seconds

        ##################################################
        # Variables
        ##################################################
        self.width = width = 320
        self.height = height = 240
        self.fps = fps = 25
        self.bars = bars = [c for y in range(height) for x in range(width) for c in [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)][x * 8 // width]]

        ##################################################
        # Blocks
        ##################################################

        self.nut_nut_sink_video_0 = nut.nut_sink(uri, 0, 0, 1, [width], [height], [str(x) for x in [fps]], command, 10.0)
        self.blocks_vector_source_x_0 = blocks.vector_source_b(bars, True, 1, [])
        self.blocks_head_0 = blocks.head(gr.sizeof_char*1, (int(seconds * fps) * width * height * 3))


        ##################################################
        # Connections
        ##################################################
        self.connect((self.blocks_head_0, 0), (self.nut_nut_sink_video_0, 0))
        self.connect((self.blocks_vector_source_x_0, 0), (self.blocks_head_0, 0))


    def get_command(self):
        return self.command

    def set_command(self, command):
        self.command = command

    def get_uri(self):
        return self.uri

    def set_uri(self, uri):
        self.uri = uri

    def get_seconds(self):
        return self.seconds

    def set_seconds(self, seconds):
        self.seconds = seconds
        self.blocks_head_0.set_length((int(self.seconds * self.fps) * self.width * self.height * 3))

    def get_width(self):
        return self.width

    def set_width(self, width):
        self.width = width
        self.set_bars([c for y in range(self.height) for x in range(self.width) for c in [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)][x * 8 // self.width]])
        self.blocks_head_0.set_length((int(self.seconds * self.fps) * self.width * self.height * 3))

    def get_height(self):
        return self.height

    def set_height(self, height):
        self.height = height
        self.set_bars([c for y in range(self.height) for x in range(self.width) for c in [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)][x * 8 // self.width]])
        self.blocks_head_0.set_length((int(self.seconds * self.fps) * self.width * self.height * 3))

    def get_fps(self):
        return self.fps

    def set_fps(self, fps):
        self.fps = fps
        self.blocks_head_0.set_length((int(self.seconds * self.fps) * self.width * self.height * 3))

    def get_bars(self):
        return self.bars

    def set_bars(self, bars):
        self.bars = bars
        self.blocks_vector_source_x_0.set_data(self.bars, [])



def argument_parser():
    description = 'nut_sink video example: a vector source holding one frame of eight vertical color bars (white/yellow/cyan/green/magenta/red/blue/black, rgb24, row-major) repeats it into the NUT Video Sink, which cuts the byte stream into frames, stamps pts from the frame count and feeds a spawned ffmpeg encoding H.264 to /tmp/bars.mkv. A K0 chain: no pacer anywhere, the graph free-runs and renders `seconds` seconds of video in a fraction of that. Play the result with any video player.'
    parser = ArgumentParser(description=description)
    parser.add_argument(
        "--command", dest="command", type=str, default='ffmpeg -y -loglevel warning -i pipe:0 -c:v libx264 -preset veryfast -pix_fmt yuv420p /tmp/bars.mkv',
        help="Set Spawn Command (spawn mode) [default=%(default)r]")
    parser.add_argument(
        "--uri", dest="uri", type=str, default='',
        help="Set NUT FIFO/file (external mode) [default=%(default)r]")
    parser.add_argument(
        "--seconds", dest="seconds", type=eng_float, default=eng_notation.num_to_str(float(5)),
        help="Set Seconds of video [default=%(default)r]")
    return parser


def main(top_block_cls=video_bars, options=None):
    if options is None:
        options = argument_parser().parse_args()
    tb = top_block_cls(command=options.command, uri=options.uri, seconds=options.seconds)

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
