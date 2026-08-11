/* -*- c++ -*- */
/*
 * Copyright 2026 Ardavast Dayleryan.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <gnuradio/io_signature.h>
#include "nut_source_impl.h"

namespace gr {
  namespace nut {

    #pragma message("set the following appropriately and remove this warning")
    using input_type = float;
    #pragma message("set the following appropriately and remove this warning")
    using output_type = float;
    nut_source::sptr
    nut_source::make(const std::string& uri, int audio_channels, int audio_rate, bool emit_video, int video_width, int video_height, bool repeat)
    {
      return gnuradio::make_block_sptr<nut_source_impl>(
        uri, audio_channels, audio_rate, emit_video, video_width, video_height, repeat);
    }


    /*
     * The private constructor
     */
    nut_source_impl::nut_source_impl(const std::string& uri, int audio_channels, int audio_rate, bool emit_video, int video_width, int video_height, bool repeat)
      : gr::block("nut_source",
              gr::io_signature::make(1 /* min inputs */, 1 /* max inputs */, sizeof(input_type)),
              gr::io_signature::make(1 /* min outputs */, 1 /*max outputs */, sizeof(output_type)))
    {}

    /*
     * Our virtual destructor.
     */
    nut_source_impl::~nut_source_impl()
    {
    }

    void
    nut_source_impl::forecast (int noutput_items, gr_vector_int &ninput_items_required)
    {
    #pragma message("implement a forecast that fills in how many items on each input you need to produce noutput_items and remove this warning")
      /* <+forecast+> e.g. ninput_items_required[0] = noutput_items */
    }

    int
    nut_source_impl::general_work (int noutput_items,
                       gr_vector_int &ninput_items,
                       gr_vector_const_void_star &input_items,
                       gr_vector_void_star &output_items)
    {
      auto in = static_cast<const input_type*>(input_items[0]);
      auto out = static_cast<output_type*>(output_items[0]);

      #pragma message("Implement the signal processing in your block and remove this warning")
      // Do <+signal processing+>
      // Tell runtime system how many input items we consumed on
      // each input stream.
      consume_each (noutput_items);

      // Tell runtime system how many output items we produced.
      return noutput_items;
    }

  } /* namespace nut */
} /* namespace gr */
