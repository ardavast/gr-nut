#
# Copyright 2008,2009 Free Software Foundation, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

# The presence of this file turns this directory into a Python package

'''
This is the GNU Radio NUT module. Place your Python package
description here (python/__init__.py).
'''
import os

# import pybind11 generated symbols into the nut namespace
try:
    # this might fail if the module is python-only
    from .nut_python import *
except ModuleNotFoundError:
    pass

# import any pure python here

try:
    _nut_sink_cxx = nut_sink  # the pybind-bound C++ block (shadowed below)
except NameError:
    pass
else:
    def nut_sink(uri, audio_channels, audio_rate, video_streams,
                 widths=(), heights=(), fps=(), command="",
                 flush_timeout=10.0):
        """Thin ergonomics wrapper over the C++ nut_sink.

        The C++ boundary takes fps as strings only (one parser, one
        policy); this wrapper coerces each entry with str() so bare
        integers and fractions.Fraction work too: str(25) -> "25",
        str(Fraction(30000, 1001)) -> "30000/1001". Floats survive the
        coercion textually (str(29.97) -> "29.97") and are still rejected
        by the strict parser — the exact-rational policy is deliberate.
        widths/heights are coerced to int lists for the same reason.
        """
        return _nut_sink_cxx(uri, int(audio_channels), int(audio_rate),
                             int(video_streams),
                             [int(w) for w in widths],
                             [int(h) for h in heights],
                             [str(f) for f in fps],
                             command, flush_timeout)
#
