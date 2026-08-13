#!/usr/bin/env python3
# -*- coding: utf-8 -*-

#
# SPDX-License-Identifier: GPL-3.0
#
# GNU Radio Python Flow Graph
# Title: gr-nut audio recording example
# Author: gr-nut
# Description: nut_sink M1 example: signal generator -> NUT Audio Sink with the default spawn command (ffmpeg records /tmp/recording.flac from NUT on its stdin). A K0 chain: no pacer anywhere, so the graph free-runs faster than real time and renders `seconds` seconds of tone in a fraction of that. On completion the sink flushes the muxer, writes the trailer, closes the pipe and WAITS for ffmpeg to finalize the file — the child exiting on its own is the normal end. External-mode variant: pass --command "" --uri FIFO and run your own ffmpeg -i FIFO ... on it.
# GNU Radio version: 3.10.9.2

from gnuradio import analog
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




class audio_record(gr.top_block):

    def __init__(self, command='ffmpeg -y -loglevel warning -i pipe:0 /tmp/recording.flac', uri='', seconds=10):
        gr.top_block.__init__(self, "gr-nut audio recording example", catch_exceptions=True)

        ##################################################
        # Parameters
        ##################################################
        self.command = command
        self.uri = uri
        self.seconds = seconds

        ##################################################
        # Variables
        ##################################################
        self.rate = rate = 48000

        ##################################################
        # Blocks
        ##################################################

        self.nut_nut_sink_audio_0 = nut.nut_sink(uri, 1, rate, 0, [], [], [], command, 10.0)
        self.blocks_head_0 = blocks.head(gr.sizeof_float*1, (int(seconds * rate)))
        self.analog_sig_source_x_0 = analog.sig_source_f(rate, analog.GR_COS_WAVE, 440, 0.5, 0, 0)


        ##################################################
        # Connections
        ##################################################
        self.connect((self.analog_sig_source_x_0, 0), (self.blocks_head_0, 0))
        self.connect((self.blocks_head_0, 0), (self.nut_nut_sink_audio_0, 0))


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
        self.blocks_head_0.set_length((int(self.seconds * self.rate)))

    def get_rate(self):
        return self.rate

    def set_rate(self, rate):
        self.rate = rate
        self.analog_sig_source_x_0.set_sampling_freq(self.rate)
        self.blocks_head_0.set_length((int(self.seconds * self.rate)))



def argument_parser():
    description = 'nut_sink M1 example: signal generator -> NUT Audio Sink with the default spawn command (ffmpeg records /tmp/recording.flac from NUT on its stdin). A K0 chain: no pacer anywhere, so the graph free-runs faster than real time and renders `seconds` seconds of tone in a fraction of that. On completion the sink flushes the muxer, writes the trailer, closes the pipe and WAITS for ffmpeg to finalize the file — the child exiting on its own is the normal end. External-mode variant: pass --command "" --uri FIFO and run your own ffmpeg -i FIFO ... on it.'
    parser = ArgumentParser(description=description)
    parser.add_argument(
        "--command", dest="command", type=str, default='ffmpeg -y -loglevel warning -i pipe:0 /tmp/recording.flac',
        help="Set Spawn Command (spawn mode) [default=%(default)r]")
    parser.add_argument(
        "--uri", dest="uri", type=str, default='',
        help="Set NUT FIFO/file (external mode) [default=%(default)r]")
    parser.add_argument(
        "--seconds", dest="seconds", type=eng_float, default=eng_notation.num_to_str(float(10)),
        help="Set Seconds to record [default=%(default)r]")
    return parser


def main(top_block_cls=audio_record, options=None):
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
