/* -*- c++ -*- */
/*
 * Copyright 2026 Ardavast Dayleryan.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INCLUDED_NUT_NUT_SOURCE_H
#define INCLUDED_NUT_NUT_SOURCE_H

#include <gnuradio/nut/api.h>
#include <gnuradio/block.h>

namespace gr {
  namespace nut {

    /*!
     * \brief <+description of block+>
     * \ingroup nut
     *
     */
    class NUT_API nut_source : virtual public gr::block
    {
     public:
      typedef std::shared_ptr<nut_source> sptr;

      /*!
       * \brief Return a shared_ptr to a new instance of nut::nut_source.
       *
       * To avoid accidental use of raw pointers, nut::nut_source's
       * constructor is in a private implementation
       * class. nut::nut_source::make is the public interface for
       * creating new instances.
       */
      static sptr make(const std::string& uri, int audio_channels, int audio_rate, bool emit_video, int video_width, int video_height, bool repeat);
    };

  } // namespace nut
} // namespace gr

#endif /* INCLUDED_NUT_NUT_SOURCE_H */
