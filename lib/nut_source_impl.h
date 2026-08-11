/* -*- c++ -*- */
/*
 * Copyright 2026 Ardavast Dayleryan.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_NUT_NUT_SOURCE_IMPL_H
#define INCLUDED_NUT_NUT_SOURCE_IMPL_H

#include <gnuradio/nut/nut_source.h>

namespace gr {
  namespace nut {

    class nut_source_impl : public nut_source
    {
     private:
      // Nothing to declare in this block.

     public:
      nut_source_impl(const std::string& uri, int audio_channels, int audio_rate, bool emit_video, int video_width, int video_height, bool repeat);
      ~nut_source_impl();

      // Where all the action really happens
      void forecast (int noutput_items, gr_vector_int &ninput_items_required);

      int general_work(int noutput_items,
           gr_vector_int &ninput_items,
           gr_vector_const_void_star &input_items,
           gr_vector_void_star &output_items);

    };

  } // namespace nut
} // namespace gr

#endif /* INCLUDED_NUT_NUT_SOURCE_IMPL_H */
