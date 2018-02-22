/* -*- c++ -*- */
/*
* Copyright (C) 2017 by Hoernchen <la@tfc-server.de>
*
* GNU Radio is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 3, or (at your option)
* any later version.
*
* GNU Radio is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with GNU Radio; see the file COPYING.  If not, write to
* the Free Software Foundation, Inc., 51 Franklin Street,
* Boston, MA 02110-1301, USA.
*/

/*
* config.h is generated by configure.  It contains the results
* of probing for features, options etc.  It should be the first
* file included in your .cc file.
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pluto_source_c.h"
#include <gnuradio/io_signature.h>
#include <volk/volk.h>

#include <boost/assign.hpp>
#include <boost/format.hpp>
#include <boost/detail/endian.hpp>
#include <boost/algorithm/string.hpp>

#include <stdexcept>
#include <iostream>
#include <stdio.h>

#include <osmoplutosdr.h>

#include "arg_helpers.h"

using namespace boost::assign;

#define BUF_LEN  (512 * 16 * 100) /* must be multiple of 512 */
#define BUF_NUM   15
#define BUF_SKIP  1 // buffers to skip due to initial garbage

#define BYTES_PER_SAMPLE  4 // rtl device delivers 8 bit unsigned IQ data

/*
 * Create a new instance of pluto_source_c and return
 * a boost shared_ptr.  This is effectively the public constructor.
 */
pluto_source_c_sptr
make_pluto_source_c (const std::string &args)
{
  return gnuradio::get_initial_sptr(new pluto_source_c (args));
}

/*
 * Specify constraints on number of input and output streams.
 * This info is used to construct the input and output signatures
 * (2nd & 3rd args to gr::block's constructor).  The input and
 * output signatures are used by the runtime system to
 * check that a valid number and type of inputs and outputs
 * are connected to this block.  In this case, we accept
 * only 0 input and 1 output.
 */
static const int MIN_IN = 0;	// mininum number of input streams
static const int MAX_IN = 0;	// maximum number of input streams
static const int MIN_OUT = 1;	// minimum number of output streams
static const int MAX_OUT = 1;	// maximum number of output streams

/*
 * The private constructor
 */
pluto_source_c::pluto_source_c (const std::string &args)
  : gr::sync_block ("pluto_source_c",
        gr::io_signature::make(MIN_IN, MAX_IN, sizeof (gr_complex)),
        gr::io_signature::make(MIN_OUT, MAX_OUT, sizeof (gr_complex))),
    _dev(NULL),
    _buf(NULL),
    _running(false),
    _auto_gain(true),
    _skipped(0)
{
  int ret;
  int index;
  unsigned int dev_index = 0;


  dict_t dict = params_to_dict(args);

  std::cerr << "Using device #" << dev_index;
  std::cerr << std::endl;

  _buf_num = _buf_len = _buf_head = _buf_used = _buf_offset = 0;

  if (dict.count("buffers"))
    _buf_num = boost::lexical_cast< unsigned int >( dict["buffers"] );

  if (dict.count("buflen"))
    _buf_len = boost::lexical_cast< unsigned int >( dict["buflen"] );

  if (0 == _buf_num)
    _buf_num = BUF_NUM;

  if (0 == _buf_len || _buf_len % 512 != 0) /* len must be multiple of 512 */
    _buf_len = BUF_LEN;

  if ( BUF_NUM != _buf_num || BUF_LEN != _buf_len ) {
    std::cerr << "Using " << _buf_num << " buffers of size " << _buf_len << "."
              << std::endl;
  }

  _samp_avail = _buf_len / BYTES_PER_SAMPLE;

  // create a lookup table for gr_complex values
  for (unsigned int i = 0; i < 0x100; i++)
    _lut.push_back((i - 127.4f) / 128.0f);

  _dev = NULL;
  ret = plutosdr_open( &_dev, dev_index );
  if (ret < 0)
    throw std::runtime_error("Failed to open rtlsdr device.");


  plutosdr_set_rfbw(_dev, 5000000);
  plutosdr_set_sample_rate(_dev, 5000000);
  
  plutosdr_set_gainctl_manual(_dev);
  plutosdr_set_gain_mdb(_dev, 0);


  plutosdr_bufstream_enable(_dev, 1);

  set_if_gain( 24 ); /* preset to a reasonable default (non-GRC use case) */

  _buf = (short **)malloc(_buf_num * sizeof(short *));

  if (_buf) {
    for(unsigned int i = 0; i < _buf_num; ++i)
      _buf[i] = (short *)malloc(_buf_len);
  }
}

/*
 * Our virtual destructor.
 */
pluto_source_c::~pluto_source_c ()
{
  if (_dev) {
    if (_running)
    {
      _running = false;
      plutosdr_cancel_async( _dev );
      _thread.join();
    }

    plutosdr_close( _dev );
    _dev = NULL;
  }

  if (_buf) {
    for(unsigned int i = 0; i < _buf_num; ++i) {
      free(_buf[i]);
    }

    free(_buf);
    _buf = NULL;
  }
}

bool pluto_source_c::start()
{
  _running = true;
  _thread = gr::thread::thread(_plutosdr_wait, this);

  return true;
}

bool pluto_source_c::stop()
{
  _running = false;
  if (_dev)
    plutosdr_cancel_async( _dev );
  _thread.join();

  return true;
}

void pluto_source_c::_plutosdr_callback(unsigned char *buf, int32_t len, void *ctx)
{
  pluto_source_c *obj = (pluto_source_c *)ctx;
  obj->plutosdr_callback(buf, len);
}

void pluto_source_c::plutosdr_callback(unsigned char *buf, int32_t len)
{
  if (_skipped < BUF_SKIP) {
    _skipped++;
    return;
  }

  {
    boost::mutex::scoped_lock lock( _buf_mutex );

    int buf_tail = (_buf_head + _buf_used) % _buf_num;
    memcpy(_buf[buf_tail], buf, len);

    if (_buf_used == _buf_num) {
      std::cerr << "O" << std::flush;
      _buf_head = (_buf_head + 1) % _buf_num;
    } else {
      _buf_used++;
    }
  }

  _buf_cond.notify_one();
}

void pluto_source_c::_plutosdr_wait(pluto_source_c *obj)
{
  obj->plutosdr_wait();
}

void pluto_source_c::plutosdr_wait()
{
  int ret = plutosdr_read_async( _dev, _plutosdr_callback, (void *)this, _buf_num, _buf_len );

  _running = false;

  if ( ret != 0 )
    std::cerr << "rtlsdr_read_async returned with " << ret << std::endl;

  _buf_cond.notify_one();
}


int pluto_source_c::work( int noutput_items,
                        gr_vector_const_void_star &input_items,
                        gr_vector_void_star &output_items )
{
  gr_complex *out = (gr_complex *)output_items[0];
  const float scaling = 2048.0f;

  {
    boost::mutex::scoped_lock lock( _buf_mutex );

    while (_buf_used < 3 && _running) // collect at least 3 buffers
      _buf_cond.wait( lock );
  }

  if (!_running)
    return WORK_DONE;

  while (noutput_items && _buf_used) {
    const int nout = std::min(noutput_items, _samp_avail);
    const short *buf = _buf[_buf_head] + _buf_offset * 2;

	volk_16i_s32f_convert_32f((float*)out, buf, scaling, 2 * nout);
	out += nout;

    noutput_items -= nout;
    _samp_avail -= nout;

    if (!_samp_avail) {
      {
        boost::mutex::scoped_lock lock( _buf_mutex );

        _buf_head = (_buf_head + 1) % _buf_num;
        _buf_used--;
      }
      _samp_avail = _buf_len / BYTES_PER_SAMPLE;
      _buf_offset = 0;
    } else {
      _buf_offset += nout;
    }
  }

  return (out - ((gr_complex *)output_items[0]));
}

std::vector<std::string> pluto_source_c::get_devices()
{
  std::vector<std::string> devices;
  std::string label;
  char manufact[256];
  char product[256];
  char serial[256];

  for (unsigned int i = 0; i < plutosdr_get_device_count(); i++) {
    std::string args = "pluto=" + boost::lexical_cast< std::string >( i );
    devices.push_back( args );
  }

  return devices;
}

size_t pluto_source_c::get_num_channels()
{
  return 1;
}

osmosdr::meta_range_t pluto_source_c::get_sample_rates()
{
  osmosdr::meta_range_t range;

  range += osmosdr::range_t( 1000000 ); // known to work
  range += osmosdr::range_t(2000000); // known to work
  range += osmosdr::range_t(3000000); // known to work
  range += osmosdr::range_t(4000000); // known to work
  range += osmosdr::range_t(5000000); // known to work
  range += osmosdr::range_t(6000000); // known to work
  range += osmosdr::range_t(7000000); // known to work
  range += osmosdr::range_t(8000000); // known to work
  range += osmosdr::range_t(9000000); // known to work
  range += osmosdr::range_t(10000000); // known to work
  range += osmosdr::range_t(11000000); // known to work

  return range;
}

double pluto_source_c::set_sample_rate(double rate)
{
  if (_dev) {
    plutosdr_set_sample_rate( _dev, (uint32_t)rate );
	}

  return get_sample_rate();
}

double pluto_source_c::get_sample_rate()
{
  return 0;
}

osmosdr::freq_range_t pluto_source_c::get_freq_range( size_t chan )
{
  osmosdr::freq_range_t range;

  if (_dev) {
      range += osmosdr::range_t( 50e6, 6000e6 );
  }

  return range;
}

double pluto_source_c::set_center_freq( double freq, size_t chan )
{
  if (_dev)
    plutosdr_set_rxlo( _dev, (uint64_t)freq );

  return get_center_freq( chan );
}

double pluto_source_c::get_center_freq( size_t chan )
{
  return 0;
}




std::vector<std::string> pluto_source_c::get_gain_names( size_t chan )
{
  std::vector< std::string > names;

  names += "AD936x";

  return names;
}

osmosdr::gain_range_t pluto_source_c::get_gain_range( size_t chan )
{
	return osmosdr::gain_range_t(-10, 77, 1);
}

osmosdr::gain_range_t pluto_source_c::get_gain_range( const std::string & name, size_t chan )
{
  return get_gain_range( chan );
}

bool pluto_source_c::set_gain_mode( bool automatic, size_t chan )
{
  return true;
}

bool pluto_source_c::get_gain_mode( size_t chan )
{
  return _auto_gain;
}

double pluto_source_c::set_gain( double gain, size_t chan )
{
  osmosdr::gain_range_t rf_gains = pluto_source_c::get_gain_range( chan );

  if (_dev) {
	  plutosdr_set_gain_mdb(_dev, rf_gains.clip(gain) * 1000);
  }

  return gain;
}

double pluto_source_c::set_gain( double gain, const std::string & name, size_t chan)
{
  return set_gain( gain, chan );
}

double pluto_source_c::get_gain( size_t chan )
{

  return 0;
}

double pluto_source_c::get_gain( const std::string & name, size_t chan )
{
  return get_gain( chan );
}


std::vector< std::string > pluto_source_c::get_antennas( size_t chan )
{
  std::vector< std::string > antennas;

  antennas += get_antenna( chan );

  return antennas;
}

std::string pluto_source_c::set_antenna( const std::string & antenna, size_t chan )
{
  return get_antenna( chan );
}

std::string pluto_source_c::get_antenna( size_t chan )
{
  return "RX";
}

double pluto_source_c::set_freq_corr(double ppm, size_t chan)
{
	return 0;
}

double pluto_source_c::get_freq_corr(size_t chan)
{
	return 0;
}

double pluto_source_c::set_bandwidth(double bandwidth, size_t chan) {
	if (_dev) {
		plutosdr_set_rfbw(_dev, (uint32_t)bandwidth);
	}
	return 0;
}