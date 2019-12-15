/* -*- c++ -*- */
/*
 * Copyright 2019 Alexandre Marquet.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gnuradio/io_signature.h>
#include "viterbi_volk_branch_impl.h"

namespace gr {
  namespace lazyviterbi {

    viterbi_volk_branch::sptr
    viterbi_volk_branch::make(const gr::trellis::fsm &FSM, int K, int S0, int SK)
    {
      return gnuradio::get_initial_sptr
        (new viterbi_volk_branch_impl(FSM, K, S0, SK));
    }

    /*
     * The private constructor
     */
    viterbi_volk_branch_impl::viterbi_volk_branch_impl(const gr::trellis::fsm &FSM, int K, int S0, int SK)
      : gr::block("viterbi_volk_branch",
              gr::io_signature::make(1, -1, sizeof(float)),
              gr::io_signature::make(1, -1, sizeof(char))),
        d_FSM(FSM), d_K(K), d_ordered_OS(FSM.S()*FSM.I())
    {
      //S0 and SK must represent a state of the trellis
      if(S0 >= 0 || S0 < d_FSM.S()) {
        d_S0 = S0;
      }
      else {
        d_S0 = -1;
      }

      if(SK >= K || SK < d_FSM.S()) {
        d_SK = SK;
      }
      else {
        d_SK = -1;
      }

      int I = d_FSM.I();
      int S = d_FSM.S();
      std::vector< std::vector<int> > PS = d_FSM.PS();
      std::vector< std::vector<int> > PI = d_FSM.PI();
      std::vector<int> OS = d_FSM.OS();

      //Compute ordered_OS and max_size_PS_s
      std::vector<int>::iterator ordered_OS_it = d_ordered_OS.begin();
      d_max_size_PS_s = 0;

      for(int s=0 ; s < S ; ++s) {
        for(size_t i=0 ; i<(PS[s]).size() ; ++i) {
          *(ordered_OS_it++) = OS[PS[s][i]*I + PI[s][i]];
        }

        if ((PS[s]).size() > d_max_size_PS_s) {
          d_max_size_PS_s = (PS[s]).size();
        }
      }

      set_relative_rate(1.0 / ((double)d_FSM.O()));
      set_output_multiple(d_K);
    }

    void
    viterbi_volk_branch_impl::set_FSM(const gr::trellis::fsm &FSM)
    {
      gr::thread::scoped_lock guard(d_setlock);
      d_FSM = FSM;
      set_relative_rate(1.0 / ((double)d_FSM.O()));
    }

    void
    viterbi_volk_branch_impl::set_K(int K)
    {
      gr::thread::scoped_lock guard(d_setlock);
      d_K = K;
      set_output_multiple(d_K);
    }

    void
    viterbi_volk_branch_impl::set_S0(int S0)
    {
      gr::thread::scoped_lock guard(d_setlock);
      d_S0 = S0;
    }

    void
    viterbi_volk_branch_impl::set_SK(int SK)
    {
      gr::thread::scoped_lock guard(d_setlock);
      d_SK = SK;
    }

    void
    viterbi_volk_branch_impl::forecast(int noutput_items, gr_vector_int &ninput_items_required)
    {
      int input_required =  d_FSM.O() * noutput_items;
      unsigned ninputs = ninput_items_required.size();
      for(unsigned int i = 0; i < ninputs; i++) {
        ninput_items_required[i] = input_required;
      }
    }

    int
    viterbi_volk_branch_impl::general_work (int noutput_items,
                       gr_vector_int &ninput_items,
                       gr_vector_const_void_star &input_items,
                       gr_vector_void_star &output_items)
    {
      gr::thread::scoped_lock guard(d_setlock);
      int nstreams = input_items.size();
      int nblocks = noutput_items / d_K;

      for(int m = 0; m < nstreams; m++) {
        const float *in = (const float*)input_items[m];
        unsigned char *out = (unsigned char*)output_items[m];

        for(int n = 0; n < nblocks; n++) {
          viterbi_algorithm_volk_branch(d_FSM.I(), d_FSM.S(), d_FSM.O(),
              d_FSM.NS(), d_ordered_OS, d_FSM.PS(), d_FSM.PI(), d_K, d_S0, d_SK,
              &(in[n*d_K*d_FSM.O()]), &(out[n*d_K]));
        }
      }

      consume_each(d_FSM.O() * noutput_items);
      return noutput_items;
    }

    //Volk optimized implementation adapted when the number of branch between
    //pairs of states is greater than the number of states.
    void
    viterbi_volk_branch_impl::viterbi_algorithm_volk_branch(int I, int S, int O,
        const std::vector<int> &NS, const std::vector<int> &ordered_OS,
        const std::vector< std::vector<int> > &PS,
        const std::vector< std::vector<int> > &PI, int K, int S0, int SK,
        const float *in, unsigned char *out)
    {
      int tb_state, pidx;
      float min_metric = std::numeric_limits<float>::max();

      std::vector<int> trace(K*S, 0);
      std::vector<float> alpha_prev(S, std::numeric_limits<float>::max());
      std::vector<float> alpha_curr(S, std::numeric_limits<float>::max());

      //Variables to be allocated by volk (for best alignment)
      float *can_vector = (float*)volk_malloc(d_max_size_PS_s*sizeof(float),
              volk_get_alignment());
      float *alpha_tmp = (float*)volk_malloc(d_max_size_PS_s*sizeof(float),
              volk_get_alignment());
      uint32_t *max_idx = (uint32_t*)volk_malloc(sizeof(uint32_t),
          volk_get_alignment());

      std::vector<float>::iterator alpha_curr_it;
      std::vector<int>::const_iterator PS_it;
      std::vector<int>::iterator trace_it = trace.begin();
      std::vector<int>::const_iterator ordered_OS_it = ordered_OS.begin();

      //If initial state was specified
      if(S0 != -1) {
        alpha_prev[S0] = 0.0;
      }
      else {
        for (std::vector<float>::iterator alpha_prev_it = alpha_prev.begin() ;
              alpha_prev_it != alpha_prev.end() ; ++alpha_prev_it) {
          *alpha_prev_it = 0.0;
        }
      }

      for(float* in_k=(float*)in ; in_k < (float*)in + K*O ; in_k += O) {
        //Current path metric iterator
        alpha_curr_it = alpha_curr.begin();
        ordered_OS_it = ordered_OS.begin();

        //Reset minimum metric (used for normalization)
        min_metric = std::numeric_limits<float>::max();

        //For each state
        for(std::vector< std::vector<int> >::const_iterator PS_s = PS.begin() ;
              PS_s != PS.end() ; ++PS_s) {

          //Iterators for previous state and previous input lists
          PS_it=(*PS_s).begin();

          //ACS for state s
          for(float* can_vector_i = can_vector ;
              can_vector_i < can_vector + (*PS_s).size() ; ++can_vector_i) {
            //can_vector_i = -in_k[OS[PS[s][i]*I + PI[s][i]]]
            *can_vector_i = -in_k[*(ordered_OS_it++)];
          }
          for(float* alpha_tmp_i = alpha_tmp ;
              alpha_tmp_i < alpha_tmp + (*PS_s).size() ; ++alpha_tmp_i) {
            //alpha_tmp[i] = -alpha_prev[PS[s][i]]
            *alpha_tmp_i = alpha_prev[*(PS_it++)];
          }
          //ADD
          volk_32f_x2_subtract_32f(can_vector, can_vector, alpha_tmp, (*PS_s).size());
          //COMPARE
          volk_32f_index_max_32u(max_idx, can_vector, (*PS_s).size());
          //SELECT
          *(alpha_curr_it++) = -can_vector[*max_idx];
          *(trace_it++) = *max_idx;

          //Update min_metric if necessary
          if(-can_vector[*max_idx] < min_metric) {
            min_metric = -can_vector[*max_idx];
          }
        }

        //Metrics normalization
        std::transform(alpha_curr.begin(), alpha_curr.end(), alpha_curr.begin(),
            std::bind2nd(std::minus<double>(), min_metric));

        //At this point, current path metrics becomes previous path metrics
        alpha_prev.swap(alpha_curr);
      }

      //Dealocate max_idx and can_vector
      volk_free(max_idx);
      volk_free(can_vector);
      volk_free(alpha_tmp);

      //If final state was specified
      if(SK != -1) {
        tb_state = SK;
      }
      else{
        //at this point, alpha_prev contains the path metrics of states after time K
        tb_state = (int)(min_element(alpha_prev.begin(), alpha_prev.end()) - alpha_prev.begin());
      }

      //Traceback
      trace_it = trace.end() - S; //place trace_it at the last time index

      for(unsigned char* out_k = out+K-1 ; out_k >= out ; --out_k) {
        //Retrieve previous input index from trace
        pidx=*(trace_it + tb_state);
        //Update trace_it for next output symbol
        trace_it -= S;

        //Output previous input
        *out_k = (unsigned char) PI[tb_state][pidx];

        //Update tb_state with the previous state on the shortest path
        tb_state = PS[tb_state][pidx];
      }
    }

  } /* namespace lazyviterbi */
} /* namespace gr */
