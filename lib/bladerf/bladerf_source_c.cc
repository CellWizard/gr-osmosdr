/* -*- c++ -*- */
/*
 * Copyright 2013-2017 Nuand LLC
 * Copyright 2013 Dimitri Stolnikov <horiz0n@gmx.net>
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

#include <iostream>

#include <boost/assign.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>

#include <gnuradio/io_signature.h>

#include <volk/volk.h>

#include "arg_helpers.h"
#include "bladerf_source_c.h"
#include "osmosdr/source.h"

using namespace boost::assign;

/******************************************************************************
 * Functions
 ******************************************************************************/

/*
 * Create a new instance of bladerf_source_c and return
 * a boost shared_ptr.  This is effectively the public constructor.
 */
bladerf_source_c_sptr make_bladerf_source_c(const std::string &args)
{
  return gnuradio::get_initial_sptr(new bladerf_source_c(args));
}

/******************************************************************************
 * Private methods
 ******************************************************************************/

/*
 * The private constructor
 */
bladerf_source_c::bladerf_source_c(const std::string &args) :
  gr::sync_block( "bladerf_source_c",
                  gr::io_signature::make(0, 0, 0),
                  args_to_io_signature(args)),
  _16icbuf(NULL),
  _32fcbuf(NULL),
  _running(false),
  _agcmode(BLADERF_GAIN_DEFAULT)
{
  int status;

  dict_t dict = params_to_dict(args);

  /* Perform src/sink agnostic initializations */
  init(dict, BLADERF_RX);

  /* Handle setting of sampling mode */
  if (dict.count("sampling")) {
    bladerf_sampling sampling = BLADERF_SAMPLING_UNKNOWN;

    if (dict["sampling"] == "internal") {
      sampling = BLADERF_SAMPLING_INTERNAL;
    } else if (dict["sampling"] == "external") {
      sampling = BLADERF_SAMPLING_EXTERNAL;
    } else {
      BLADERF_WARNING("Invalid sampling mode: " + dict["sampling"]);
    }

    if (sampling != BLADERF_SAMPLING_UNKNOWN) {
      status = bladerf_set_sampling(_dev.get(), sampling);
      if (status != 0) {
        BLADERF_WARNING("Problem while setting sampling mode: " <<
                        bladerf_strerror(status));
      }
    }
  }

  /* Bias tee */
  if (dict.count("biastee")) {
    set_biastee_mode(dict["biastee"]);
  }

  /* Loopback */
  set_loopback_mode(dict.count("loopback") ? dict["loopback"] : "none");

  /* RX Mux */
  set_rx_mux_mode(dict.count("rxmux") ? dict["rxmux"] : "baseband");

  /* AGC mode */
  if (dict.count("agc_mode")) {
    set_agc_mode(dict["agc_mode"]);
  }

  /* Specify initial gain mode */
  if (dict.count("agc")) {
    for (size_t i = 0; i < get_max_channels(); ++i) {
      set_gain_mode(boost::lexical_cast<bool>(dict["agc"]), BLADERF_CHANNEL_RX(i));
      BLADERF_INFO(boost::str(boost::format("%s gain mode set to '%s'")
                    % channel2str(BLADERF_CHANNEL_RX(i))
                    % get_gain_mode(BLADERF_CHANNEL_RX(i))));
    }
  }

  /* Warn user about using an old FPGA version, as we no longer strip off the
   * markers that were pressent in the pre-v0.0.1 FPGA */
  {
    struct bladerf_version fpga_version;

    if (bladerf_fpga_version(_dev.get(), &fpga_version) != 0) {
      BLADERF_WARNING("Failed to get FPGA version");
    } else if (fpga_version.major <= 0 &&
               fpga_version.minor <= 0 &&
               fpga_version.patch < 1) {
      BLADERF_WARNING("Warning: FPGA version v0.0.1 or later is required. "
                      "Using an earlier FPGA version will result in "
                      "misinterpeted samples.");
    }
  }

  /* Initialize channel <-> antenna map */
  BOOST_FOREACH(std::string ant, get_antennas()) {
    _chanmap[str2channel(ant)] = -1;
  }

  /* Bounds-checking output signature depending on our underlying hardware */
  if (get_num_channels() > get_max_channels()) {
    BLADERF_WARNING("Warning: number of channels specified on command line ("
                    << get_num_channels() << ") is greater than the maximum "
                    "number supported by this device (" << get_max_channels()
                    << "). Resetting to " << get_max_channels() << ".");

    set_output_signature(gr::io_signature::make(get_max_channels(),
                                                get_max_channels(),
                                                sizeof(gr_complex)));
  }

  /* Set up constraints */
  int const alignment_multiple = volk_get_alignment() / sizeof(gr_complex);
  set_alignment(std::max(1,alignment_multiple));
  set_max_noutput_items(_samples_per_buffer);
  set_output_multiple(get_num_channels());

  /* Set channel layout */
  _layout = (get_num_channels() > 1) ? BLADERF_RX_X2 : BLADERF_RX_X1;

  /* Initial wiring of antennas to channels */
  for (size_t ch = 0; ch < get_num_channels(); ++ch) {
    set_channel_enable(BLADERF_CHANNEL_RX(ch), true);
    _chanmap[BLADERF_CHANNEL_RX(ch)] = ch;
  }

  BLADERF_DEBUG("initialization complete");
}

bool bladerf_source_c::is_antenna_valid(const std::string &antenna)
{
  BOOST_FOREACH(std::string ant, get_antennas()) {
    if (antenna == ant) {
      return true;
    }
  }

  return false;
}

/******************************************************************************
 * Public methods
 ******************************************************************************/

std::string bladerf_source_c::name()
{
  return "bladeRF receiver";
}

std::vector<std::string> bladerf_source_c::get_devices()
{
  return bladerf_common::devices();
}

size_t bladerf_source_c::get_max_channels()
{
  return bladerf_common::get_max_channels(BLADERF_RX);
}

size_t bladerf_source_c::get_num_channels()
{
  return output_signature()->max_streams();
}

bool bladerf_source_c::start()
{
  int status;

  BLADERF_DEBUG("starting source");

  gr::thread::scoped_lock guard(d_mutex);

  status = bladerf_sync_config(_dev.get(), _layout, _format, _num_buffers,
                               _samples_per_buffer, _num_transfers,
                               _stream_timeout);
  if (status != 0) {
    BLADERF_THROW_STATUS(status, "bladerf_sync_config failed");
  }

  for (size_t ch = 0; ch < get_max_channels(); ++ch) {
    bladerf_channel brfch = BLADERF_CHANNEL_RX(ch);
    if (get_channel_enable(brfch)) {
      status = bladerf_enable_module(_dev.get(), brfch, true);
      if (status != 0) {
        BLADERF_THROW_STATUS(status, "bladerf_enable_module failed");
      }
    }
  }

  /* Allocate memory for conversions in work() */
  size_t alignment = volk_get_alignment();

  _16icbuf = reinterpret_cast<int16_t *>(volk_malloc(2*_samples_per_buffer*sizeof(int16_t), alignment));
  _32fcbuf = reinterpret_cast<gr_complex *>(volk_malloc(_samples_per_buffer*sizeof(gr_complex), alignment));
  bladerf_set_rfic_register(_dev.get(),0x003,0x54);
  bladerf_set_rfic_register(_dev.get(),0x1e0,0xBF);
  bladerf_set_rfic_register(_dev.get(),0x1e4,0xFF);
  bladerf_set_rfic_register(_dev.get(),0x1f2,0xFF);
  bladerf_set_rfic_register(_dev.get(),0x1e6,0x87);
  bladerf_set_rfic_register(_dev.get(),0x1e7,0x00);
  bladerf_set_rfic_register(_dev.get(),0x1e8,0x00);
  bladerf_set_rfic_register(_dev.get(),0x1e9,0x00);
  bladerf_set_rfic_register(_dev.get(),0x1ea,0x00);
  bladerf_set_rfic_register(_dev.get(),0x1eb,0x00);
  bladerf_set_rfic_register(_dev.get(),0x1ec,0x00);
  bladerf_set_rfic_register(_dev.get(),0x1ed,0x00);
  bladerf_set_rfic_register(_dev.get(),0x1ee,0x00);
  bladerf_set_rfic_register(_dev.get(),0x1ef,0x00); 
  bladerf_set_rfic_register(_dev.get(),0x1e0,0xBF);
  bladerf_set_rfic_register(_dev.get(),0x1e4,0xFF);
  bladerf_set_rfic_register(_dev.get(),0x1f2,0xFF);
  bladerf_set_rfic_register(_dev.get(),0x1e6,0x87);
  bladerf_set_rfic_register(_dev.get(),0x1e7,0x00);
  bladerf_set_rfic_register(_dev.get(),0x1e8,0x00);
  bladerf_set_rfic_register(_dev.get(),0x1e9,0x00);
  bladerf_set_rfic_register(_dev.get(),0x1ea,0x00);
  bladerf_set_rfic_register(_dev.get(),0x1eb,0x00);
  bladerf_set_rfic_register(_dev.get(),0x1ec,0x00);
  bladerf_set_rfic_register(_dev.get(),0x1ed,0x00);
  bladerf_set_rfic_register(_dev.get(),0x1ee,0x00);
  bladerf_set_rfic_register(_dev.get(),0x1ef,0x00);
  bladerf_set_rfic_register(_dev.get(),0x3f6,0x03);
  _running = true;

  return true;
}

bool bladerf_source_c::stop()
{
  int status;

  BLADERF_DEBUG("stopping source");

  gr::thread::scoped_lock guard(d_mutex);

  if (!_running) {
    BLADERF_WARNING("source already stopped, nothing to do here");
    return true;
  }

  _running = false;

  for (size_t ch = 0; ch < get_max_channels(); ++ch) {
    bladerf_channel brfch = BLADERF_CHANNEL_RX(ch);
    if (get_channel_enable(brfch)) {
      status = bladerf_enable_module(_dev.get(), brfch, false);
      if (status != 0) {
        BLADERF_THROW_STATUS(status, "bladerf_enable_module failed");
      }
    }
  }

  /* Deallocate conversion memory */
  volk_free(_16icbuf);
  volk_free(_32fcbuf);
  _16icbuf = NULL;
  _32fcbuf = NULL;

  return true;
}

int bladerf_source_c::work(int noutput_items,
                          gr_vector_const_void_star &input_items,
                          gr_vector_void_star &output_items)
{
  int status;
  struct bladerf_metadata meta;
  struct bladerf_metadata *meta_ptr = NULL;
  size_t nstreams = num_streams(_layout);

  gr::thread::scoped_lock guard(d_mutex);

  // if we aren't running, nothing to do here
  if (!_running) {
    return 0;
  }

  // set up metadata
  if (BLADERF_FORMAT_SC16_Q11_META == _format) {
    memset(&meta, 0, sizeof(meta));
    meta.flags = BLADERF_META_FLAG_RX_NOW;
    meta_ptr = &meta;
  }

  // grab samples into temp buffer
  status = bladerf_sync_rx(_dev.get(), static_cast<void *>(_16icbuf),
                           noutput_items, meta_ptr, _stream_timeout);
  if (status != 0) {
    BLADERF_WARNING(boost::str(boost::format("bladerf_sync_rx error: %s")
                    % bladerf_strerror(status)));
    ++_failures;

    if (_failures >= MAX_CONSECUTIVE_FAILURES) {
      BLADERF_WARNING("Consecutive error limit hit. Shutting down.");
      return WORK_DONE;
    }
  } else {
    _failures = 0;
  }
  for (int i=0; i<noutput_items; i+=2) {
        float *fbuf=reinterpret_cast<float *>(_32fcbuf);
        fbuf[i*2+0]=((float)((int8_t)((_16icbuf[i+0]&0xFF))))/127.0;// | ((fbuf[(i*2)+2]*127)<<8);
        fbuf[i*2+1]=((float)((int8_t)((_16icbuf[i+1]&0xFF))))/127.0;// | ((fbuf[(i*2)+2]*127)<<8);
        fbuf[i*2+2]=((float)((int8_t)((_16icbuf[i+0]>>8)&0xFF)))/127.0;// | ((fbuf[(i*2)+2]*127)<<8);
        fbuf[i*2+3]=((float)((int8_t)((_16icbuf[i+1]>>8)&0xFF)))/127.0;// | ((fbuf[(i*2)+2]*127)<<8);
  }
  // convert from int16_t to float
  // output_items is gr_complex (2x float), so num_points is 2*noutput_items
//  volk_16i_s32f_convert_32f(reinterpret_cast<float *>(_32fcbuf), _16icbuf,
//                            SCALING_FACTOR, 2*noutput_items);

  // copy the samples into output_items
  gr_complex **out = reinterpret_cast<gr_complex **>(&output_items[0]);

  if (nstreams > 1) {
    // we need to deinterleave the multiplex as we copy
    gr_complex const *deint_in = _32fcbuf;

    for (size_t i = 0; i < (noutput_items/nstreams); ++i) {
      for (size_t n = 0; n < nstreams; ++n) {
        memcpy(out[n]++, deint_in++, sizeof(gr_complex));
      }
    }
  } else {
    // no deinterleaving to do: simply copy everything
    memcpy(out[0], _32fcbuf, sizeof(gr_complex) * noutput_items);
  }

  return noutput_items;
}

osmosdr::meta_range_t bladerf_source_c::get_sample_rates()
{
  return sample_rates(chan2channel(BLADERF_RX, 0));
}

double bladerf_source_c::set_sample_rate(double rate)
{
  return bladerf_common::set_sample_rate(rate, chan2channel(BLADERF_RX, 0));
}

double bladerf_source_c::get_sample_rate()
{
  return bladerf_common::get_sample_rate(chan2channel(BLADERF_RX, 0));
}

osmosdr::freq_range_t bladerf_source_c::get_freq_range(size_t chan)
{
  return bladerf_common::freq_range(chan2channel(BLADERF_RX, chan));
}

double bladerf_source_c::set_center_freq(double freq, size_t chan)
{
  return bladerf_common::set_center_freq(freq, chan2channel(BLADERF_RX, chan));
}

double bladerf_source_c::get_center_freq(size_t chan)
{
  return bladerf_common::get_center_freq(chan2channel(BLADERF_RX, chan));
}

double bladerf_source_c::set_freq_corr(double ppm, size_t chan)
{
  /* TODO: Write the VCTCXO with a correction value (also changes TX ppm value!) */
  BLADERF_WARNING("Frequency correction is not implemented.");
  return get_freq_corr(chan2channel(BLADERF_RX, chan));
}

double bladerf_source_c::get_freq_corr(size_t chan)
{
  /* TODO: Return back the frequency correction in ppm */
  return 0;
}

std::vector<std::string> bladerf_source_c::get_gain_names(size_t chan)
{
  return bladerf_common::get_gain_names(chan2channel(BLADERF_RX, chan));
}

osmosdr::gain_range_t bladerf_source_c::get_gain_range(size_t chan)
{
  return bladerf_common::get_gain_range(chan2channel(BLADERF_RX, chan));
}

osmosdr::gain_range_t bladerf_source_c::get_gain_range(const std::string &name,
                                                       size_t chan)
{
  return bladerf_common::get_gain_range(name, chan2channel(BLADERF_RX, chan));
}

bool bladerf_source_c::set_gain_mode(bool automatic, size_t chan)
{
  return bladerf_common::set_gain_mode(automatic,
                                       chan2channel(BLADERF_RX, chan),
                                       _agcmode);
}

bool bladerf_source_c::get_gain_mode(size_t chan)
{
  return bladerf_common::get_gain_mode(chan2channel(BLADERF_RX, chan));
}

double bladerf_source_c::set_gain(double gain, size_t chan)
{
  return bladerf_common::set_gain(gain, chan2channel(BLADERF_RX, chan));
}

double bladerf_source_c::set_gain(double gain, const std::string &name,
                                  size_t chan)
{
  return bladerf_common::set_gain(gain, name, chan2channel(BLADERF_RX, chan));
}

double bladerf_source_c::get_gain(size_t chan)
{
  return bladerf_common::get_gain(chan2channel(BLADERF_RX, chan));
}

double bladerf_source_c::get_gain(const std::string &name, size_t chan)
{
  return bladerf_common::get_gain(name, chan2channel(BLADERF_RX, chan));
}

std::vector<std::string> bladerf_source_c::get_antennas(size_t chan)
{
  return bladerf_common::get_antennas(BLADERF_RX);
}

std::string bladerf_source_c::set_antenna(const std::string &antenna,
                                          size_t chan)
{
  bool _was_running = _running;

  if (_was_running) {
    stop();
  }

  bladerf_common::set_antenna(BLADERF_RX, chan, antenna);

  if (_was_running) {
    start();
  }

  return get_antenna(chan);
}

std::string bladerf_source_c::get_antenna(size_t chan)
{
  return channel2str(chan2channel(BLADERF_RX, chan));
}

void bladerf_source_c::set_dc_offset_mode(int mode, size_t chan)
{
  if (osmosdr::source::DCOffsetOff == mode) {
    //_src->set_auto_dc_offset( false, chan );
    /* reset to default for off-state */
    set_dc_offset(std::complex<double>(0.0, 0.0), chan);
  } else if (osmosdr::source::DCOffsetManual == mode) {
    /* disable auto mode, but keep correcting with last known values */
    //_src->set_auto_dc_offset( false, chan );
  } else if (osmosdr::source::DCOffsetAutomatic == mode) {
    //_src->set_auto_dc_offset( true, chan );
    BLADERF_WARNING("Automatic DC correction mode is not implemented.");
  }
}

void bladerf_source_c::set_dc_offset(const std::complex<double> &offset,
                                     size_t chan)
{
  int status;

  status = bladerf_common::set_dc_offset(offset, chan2channel(BLADERF_RX, chan));

  if (status != 0) {
    BLADERF_THROW_STATUS(status, "could not set dc offset");
  }
}

void bladerf_source_c::set_iq_balance_mode(int mode, size_t chan)
{
  if (osmosdr::source::IQBalanceOff == mode) {
    //_src->set_auto_iq_balance( false, chan );
    /* reset to default for off-state */
    set_iq_balance(std::complex<double>(0.0, 0.0), chan);
  } else if (osmosdr::source::IQBalanceManual == mode) {
    /* disable auto mode, but keep correcting with last known values */
    //_src->set_auto_iq_balance( false, chan );
  } else if (osmosdr::source::IQBalanceAutomatic == mode) {
    //_src->set_auto_iq_balance( true, chan );
    BLADERF_WARNING("Automatic IQ correction mode is not implemented.");
  }
}

void bladerf_source_c::set_iq_balance(const std::complex<double> &balance,
                                      size_t chan)
{
  int status;

  status = bladerf_common::set_iq_balance(balance, chan2channel(BLADERF_RX, chan));

  if (status != 0) {
    BLADERF_THROW_STATUS(status, "could not set iq balance");
  }
}

osmosdr::freq_range_t bladerf_source_c::get_bandwidth_range(size_t chan)
{
  return filter_bandwidths(chan2channel(BLADERF_RX, chan));
}

double bladerf_source_c::set_bandwidth(double bandwidth, size_t chan)
{
  return bladerf_common::set_bandwidth(bandwidth,
                                       chan2channel(BLADERF_RX, chan));
}

double bladerf_source_c::get_bandwidth(size_t chan)
{
  return bladerf_common::get_bandwidth(chan2channel(BLADERF_RX, chan));
}

std::vector<std::string> bladerf_source_c::get_clock_sources(size_t mboard)
{
  return bladerf_common::get_clock_sources(mboard);
}

void bladerf_source_c::set_clock_source(const std::string &source,
                                        size_t mboard)
{
  bladerf_common::set_clock_source(source, mboard);
}

std::string bladerf_source_c::get_clock_source(size_t mboard)
{
  return bladerf_common::get_clock_source(mboard);
}

void bladerf_source_c::set_biastee_mode(const std::string &mode)
{
  int status;
  bool enable;

  if (mode == "on" || mode == "1" || mode == "rx") {
    enable = true;
  } else {
    enable = false;
  }

  status = bladerf_set_bias_tee(_dev.get(), BLADERF_CHANNEL_RX(0), enable);
  if (BLADERF_ERR_UNSUPPORTED == status) {
    // unsupported, but not worth crashing out
    BLADERF_WARNING("Bias-tee not supported by device");
  } else if (status != 0) {
    BLADERF_THROW_STATUS(status, "Failed to set bias-tee");
  }
}

void bladerf_source_c::set_loopback_mode(const std::string &loopback)
{
  int status;
  bladerf_loopback mode;

  if (loopback == "bb_txlpf_rxvga2") {
    mode = BLADERF_LB_BB_TXLPF_RXVGA2;
  } else if (loopback == "bb_txlpf_rxlpf") {
    mode = BLADERF_LB_BB_TXLPF_RXLPF;
  } else if (loopback == "bb_txvga1_rxvga2") {
    mode = BLADERF_LB_BB_TXVGA1_RXVGA2;
  } else if (loopback == "bb_txvga1_rxlpf") {
    mode = BLADERF_LB_BB_TXVGA1_RXLPF;
  } else if (loopback == "rf_lna1") {
    mode = BLADERF_LB_RF_LNA1;
  } else if (loopback == "rf_lna2") {
    mode = BLADERF_LB_RF_LNA2;
  } else if (loopback == "rf_lna3") {
    mode = BLADERF_LB_RF_LNA3;
  } else if (loopback == "firmware") {
    mode = BLADERF_LB_FIRMWARE;
  } else if (loopback == "rfic_bist") {
    mode = BLADERF_LB_RFIC_BIST;
  } else if (loopback == "none") {
    mode = BLADERF_LB_NONE;
  } else {
    BLADERF_THROW("Unknown loopback mode: " + loopback);
  }

  status = bladerf_set_loopback(_dev.get(), mode);
  if (BLADERF_ERR_UNSUPPORTED == status) {
    // unsupported, but not worth crashing out
    BLADERF_WARNING("Loopback mode not supported by device: " + loopback);
  } else if (status != 0) {
    BLADERF_THROW_STATUS(status, "Failed to set loopback mode");
  }
}

void bladerf_source_c::set_rx_mux_mode(const std::string &rxmux)
{
  int status;
  bladerf_rx_mux mode;

  if (rxmux == "baseband") {
    mode = BLADERF_RX_MUX_BASEBAND;
  } else if (rxmux == "12bit") {
    mode = BLADERF_RX_MUX_12BIT_COUNTER;
  } else if (rxmux == "32bit") {
    mode = BLADERF_RX_MUX_32BIT_COUNTER;
  } else if (rxmux == "digital") {
    mode = BLADERF_RX_MUX_DIGITAL_LOOPBACK;
  } else {
    BLADERF_THROW("Unknown RX mux mode: " + rxmux);
  }

  status = bladerf_set_rx_mux(_dev.get(), mode);
  if (BLADERF_ERR_UNSUPPORTED == status) {
    // unsupported, but not worth crashing out
    BLADERF_WARNING("RX mux mode not supported by device: " + rxmux);
  } else if (status != 0) {
    BLADERF_THROW_STATUS(status, "Failed to set RX mux mode");
  }
}

void bladerf_source_c::set_agc_mode(const std::string &agcmode)
{
#ifndef BLADERF_COMPATIBILITY
  int status;
  bladerf_gain_mode mode;
  bool ok = false;
  struct bladerf_gain_modes const *modes = NULL;

  /* Get the list of AGC modes */
  status = bladerf_get_gain_modes(_dev.get(), BLADERF_CHANNEL_RX(0), &modes);
  if (status < 0) {
    BLADERF_THROW_STATUS(status, "failed to get gain modes");
  }

  size_t count = status;

  /* Compare... */
  for (size_t i = 0; i < count; ++i) {
    if (agcmode == std::string(modes[i].name)) {
      mode = modes[i].mode;
      ok = true;
      BLADERF_DEBUG("Setting gain mode to " << mode << " (" << agcmode << ")");
      break;
    }
  }

  if (!ok) {
    BLADERF_WARNING("Unknown gain mode \"" << agcmode << "\"");
    return;
  }

  _agcmode = mode;

  for (size_t i = 0; i < get_num_channels(); ++i) {
    if (bladerf_common::get_gain_mode(BLADERF_CHANNEL_RX(i))) {
      /* Refresh this */
      bladerf_common::set_gain_mode(true, BLADERF_CHANNEL_RX(i), _agcmode);
    }
  }
#endif
}
