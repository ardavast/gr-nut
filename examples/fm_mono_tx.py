#!/usr/bin/env python3
# -*- coding: utf-8 -*-

#
# SPDX-License-Identifier: GPL-3.0
#
# GNU Radio Python Flow Graph
# Title: gr-nut mono FM transmitter
# Author: gr-nut
# Description: mono FM transmitter: NUT ingest -> 48k -> 200k (25/6) -> WBFM mono (+-75 kHz, 50 us preemphasis) -> 200k -> 8M (40/1) -> HackRF. The HackRF is the only clock; ffmpeg is paced by pipe backpressure. No Throttle. Spawn mode showcase: pass --command "ffmpeg -i song.flac -vn -af aresample=48000:async=1 -ac 1 -c:a pcm_f32le -max_interleave_delta 500000 -f nut pipe:1" and the block runs ffmpeg itself. Ops variant: fm_mono_tx.sh starts ffmpeg externally and passes --uri FIFO instead.
# GNU Radio version: 3.10.9.2

from gnuradio import analog
from gnuradio import filter
from gnuradio.filter import firdes
from gnuradio import gr
from gnuradio.fft import window
import sys
import signal
from argparse import ArgumentParser
from gnuradio.eng_arg import eng_float, intx
from gnuradio import eng_notation
from gnuradio import nut
from gnuradio import soapy




class fm_mono_tx(gr.top_block):

    def __init__(self, uri='', command='ffmpeg -i song.flac -vn -af aresample=48000:async=1 -ac 1 -c:a pcm_f32le -max_interleave_delta 500000 -f nut pipe:1', freq=99.9e6, vga_gain=40, amp=0):
        gr.top_block.__init__(self, "gr-nut mono FM transmitter", catch_exceptions=True)

        ##################################################
        # Parameters
        ##################################################
        self.uri = uri
        self.command = command
        self.freq = freq
        self.vga_gain = vga_gain
        self.amp = amp

        ##################################################
        # Blocks
        ##################################################

        self.soapy_hackrf_sink_0 = None
        dev = 'driver=hackrf'
        stream_args = ''
        tune_args = ['']
        settings = ['']

        self.soapy_hackrf_sink_0 = soapy.sink(dev, "fc32", 1, '',
                                  stream_args, tune_args, settings)
        self.soapy_hackrf_sink_0.set_sample_rate(0, 8e6)
        self.soapy_hackrf_sink_0.set_bandwidth(0, 0)
        self.soapy_hackrf_sink_0.set_frequency(0, freq)
        self.soapy_hackrf_sink_0.set_gain(0, 'AMP', bool(amp))
        self.soapy_hackrf_sink_0.set_gain(0, 'VGA', min(max(vga_gain, 0.0), 47.0))
        self.rational_resampler_xxx_1 = filter.rational_resampler_ccc(
                interpolation=40,
                decimation=1,
                taps=[],
                fractional_bw=0)
        self.rational_resampler_xxx_0 = filter.rational_resampler_fff(
                interpolation=25,
                decimation=6,
                taps=[],
                fractional_bw=0)
        self.nut_nut_source_audio_0 = nut.nut_source(uri, 1, 0, command)
        self.analog_wfm_tx_0 = analog.wfm_tx(
        	audio_rate=200000,
        	quad_rate=200000,
        	tau=(50e-6),
        	max_dev=75e3,
        	fh=(-1.0),
        )


        ##################################################
        # Connections
        ##################################################
        self.connect((self.analog_wfm_tx_0, 0), (self.rational_resampler_xxx_1, 0))
        self.connect((self.nut_nut_source_audio_0, 0), (self.rational_resampler_xxx_0, 0))
        self.connect((self.rational_resampler_xxx_0, 0), (self.analog_wfm_tx_0, 0))
        self.connect((self.rational_resampler_xxx_1, 0), (self.soapy_hackrf_sink_0, 0))


    def get_uri(self):
        return self.uri

    def set_uri(self, uri):
        self.uri = uri

    def get_command(self):
        return self.command

    def set_command(self, command):
        self.command = command

    def get_freq(self):
        return self.freq

    def set_freq(self, freq):
        self.freq = freq
        self.soapy_hackrf_sink_0.set_frequency(0, self.freq)

    def get_vga_gain(self):
        return self.vga_gain

    def set_vga_gain(self, vga_gain):
        self.vga_gain = vga_gain
        self.soapy_hackrf_sink_0.set_gain(0, 'VGA', min(max(self.vga_gain, 0.0), 47.0))

    def get_amp(self):
        return self.amp

    def set_amp(self, amp):
        self.amp = amp
        self.soapy_hackrf_sink_0.set_gain(0, 'AMP', bool(self.amp))



def argument_parser():
    description = 'mono FM transmitter: NUT ingest -> 48k -> 200k (25/6) -> WBFM mono (+-75 kHz, 50 us preemphasis) -> 200k -> 8M (40/1) -> HackRF. The HackRF is the only clock; ffmpeg is paced by pipe backpressure. No Throttle. Spawn mode showcase: pass --command "ffmpeg -i song.flac -vn -af aresample=48000:async=1 -ac 1 -c:a pcm_f32le -max_interleave_delta 500000 -f nut pipe:1" and the block runs ffmpeg itself. Ops variant: fm_mono_tx.sh starts ffmpeg externally and passes --uri FIFO instead.'
    parser = ArgumentParser(description=description)
    parser.add_argument(
        "--uri", dest="uri", type=str, default='',
        help="Set NUT FIFO/file (external mode) [default=%(default)r]")
    parser.add_argument(
        "--command", dest="command", type=str, default='ffmpeg -i song.flac -vn -af aresample=48000:async=1 -ac 1 -c:a pcm_f32le -max_interleave_delta 500000 -f nut pipe:1',
        help="Set Spawn Command (spawn mode) [default=%(default)r]")
    parser.add_argument(
        "--freq", dest="freq", type=eng_float, default=eng_notation.num_to_str(float(99.9e6)),
        help="Set Center Frequency (Hz) [default=%(default)r]")
    parser.add_argument(
        "--vga-gain", dest="vga_gain", type=eng_float, default=eng_notation.num_to_str(float(40)),
        help="Set HackRF VGA Gain (0-47 dB) [default=%(default)r]")
    parser.add_argument(
        "--amp", dest="amp", type=intx, default=0,
        help="Set HackRF Amp (0/1, +14 dB) [default=%(default)r]")
    return parser


def main(top_block_cls=fm_mono_tx, options=None):
    if options is None:
        options = argument_parser().parse_args()
    tb = top_block_cls(uri=options.uri, command=options.command, freq=options.freq, vga_gain=options.vga_gain, amp=options.amp)

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
