/* -*- c++ -*- */
/*
 * Copyright 2017-2018 Free Software Foundation, Inc.
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
#include "lazy_viterbi_impl.h"

namespace gr {
  namespace lazyviterbi {

    lazy_viterbi::sptr
    lazy_viterbi::make(const gr::trellis::fsm &FSM, int K, int S0, int SK)
    {
      return gnuradio::get_initial_sptr
        (new lazy_viterbi_impl(FSM, K, S0, SK));
    }

    /*
     * The private constructor
     */
    lazy_viterbi_impl::lazy_viterbi_impl(const gr::trellis::fsm &FSM, int K, int S0, int SK)
      : gr::block("lazy_viterbi",
              gr::io_signature::make(1, -1, sizeof(float)),
              gr::io_signature::make(1, -1, sizeof(char))),
        d_FSM(FSM), d_K(K), d_metrics(K*d_FSM.O())
    {
      struct node new_node = {0, -1, false}; //{prev_state_idx, prev_input, expanded}

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

      //Allocate expanded and shadow nodes containers
      d_shadow_nodes.resize(256);  //256=2^8=2^sizeof(uint8_t)
      d_real_nodes.resize((d_K+1)*d_FSM.S());
      //Set all real nodes to non-expanded
      for(std::vector<node>::iterator it=d_real_nodes.begin() ; it != d_real_nodes.end() ; ++it) {
        (*it).expanded=false;
      }

      set_relative_rate(1.0 / ((double)d_FSM.O()));
      set_output_multiple(d_K);
    }

    void
    lazy_viterbi_impl::set_S0(int S0)
    {
      gr::thread::scoped_lock guard(d_setlock);
      d_S0 = S0;
    }

    void
    lazy_viterbi_impl::set_SK(int SK)
    {
      gr::thread::scoped_lock guard(d_setlock);
      d_SK = SK;
    }

    void
    lazy_viterbi_impl::forecast(int noutput_items, gr_vector_int &ninput_items_required)
    {
      int input_required =  d_FSM.O() * noutput_items;
      unsigned ninputs = ninput_items_required.size();
      for(unsigned int i = 0; i < ninputs; i++) {
        ninput_items_required[i] = input_required;
      }
    }

    int
    lazy_viterbi_impl::general_work (int noutput_items,
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
          lazy_viterbi_algorithm(d_FSM.I(), d_FSM.S(), d_FSM.O(), d_FSM.NS(),
              d_FSM.OS(), d_K, d_S0, d_SK, &(in[n*d_K*d_FSM.O()]), &(out[n*d_K]));
        }
      }

      consume_each(d_FSM.O() * noutput_items);
      return noutput_items;
    }

    struct minus_cast : public std::binary_function<float, float, uint8_t> {
        uint8_t operator() (float a, float b) const {return (uint8_t)(a-b);}
    };

    void
    lazy_viterbi_impl::lazy_viteri_metrics_norm(const float *in, uint8_t* metrics,
        int K, int O)
    {
      float min_metric = 0;

      for(float *in_k=(float*)in ; in_k < in + K*O ; in_k += O) {
        //Find min_element
        min_metric=*std::min_element(in_k, in_k+O);

        //Remove it from metrics
        std::transform(in_k, in_k+O, metrics, std::bind2nd(minus_cast(), min_metric));

        metrics += O;
      }
    }

    void
    lazy_viterbi_impl::lazy_viterbi_algorithm(int I, int S, int O, const std::vector<int> &NS,
        const std::vector<int> &OS, int K, int S0, int SK, const float *in,
        unsigned char *out)
    {
      //***INIT***//
      std::vector<uint8_t>::iterator metrics_os_it;
      uint8_t min_dist_idx = 0;
      struct node new_node;
      struct shadow_node new_shadow, curr_shadow;
      std::vector<node>::iterator expanded_it;
      std::vector<int>::const_iterator NS_it, OS_it;

      //If exist put initial node in the shadow queue,
      //otherwise, put every nodes a time_idx==0 in it
      if(S0 != -1) {
        new_shadow.time_idx=0;
        new_shadow.state_idx=S0;
        new_shadow.prev_state_idx=0;
        new_shadow.prev_input=-1;

        d_shadow_nodes[0].push_back(new_shadow);
      }
      else {
        //For each state
        for(int s=0 ; s < S ; ++s) {
          new_shadow.time_idx=0;
          new_shadow.state_idx=s;
          new_shadow.prev_state_idx=0;
          new_shadow.prev_input=-1;

          d_shadow_nodes[0].push_back(new_shadow);
        }
      }

      //***NORMALIZE METRICS***//
      lazy_viteri_metrics_norm(in, &d_metrics[0], K, O);

      //***FIND SHORTEST PATH***//
      do {
        //Select another candidate if this node has already been expanded
        do {
          //Find minimum distance index
          while(d_shadow_nodes[min_dist_idx].empty()) {
            ++min_dist_idx;
          }

          //Retrieve a candidate at minimum distance
          curr_shadow = d_shadow_nodes[min_dist_idx].back();
          d_shadow_nodes[min_dist_idx].pop_back();

          //Update iterator
          expanded_it = d_real_nodes.begin() + curr_shadow.time_idx*S
            + curr_shadow.state_idx;
        } while((*expanded_it).expanded);

        //At this point, we are sure curr_shadow will be expanded
        (*expanded_it).expanded=true;
        (*expanded_it).prev_input=curr_shadow.prev_input;
        (*expanded_it).prev_state_idx=curr_shadow.prev_state_idx;

        //Scan all neighbors of the last expanded node
        //Create a shadow neighbor node (pt 1)
        new_shadow.time_idx=curr_shadow.time_idx+1;
        new_shadow.prev_state_idx=curr_shadow.state_idx;

        //Initialize iterators
        expanded_it += S - curr_shadow.state_idx; //real_nodes[curr_shadow.time_idx*S]
        metrics_os_it = d_metrics.begin() + curr_shadow.time_idx*O; //metrics[curr_shadow.time_idx*O]
        NS_it = NS.begin() + curr_shadow.state_idx*I; //NS[curr_shadow.state_idx*I]
        OS_it = OS.begin() + curr_shadow.state_idx*I; //OS[curr_shadow.state_idx*I]

        //For all neighbors
        for(int i=0 ; i < I ; ++i) {
          //Create a shadow neighbor node (pt 2)
          new_shadow.state_idx=*NS_it;
          new_shadow.prev_input=i;

          //Add non-expanded neighbors as shadow nodes
          if((*(expanded_it + new_shadow.state_idx)).expanded == false) {
            d_shadow_nodes[(uint8_t)(min_dist_idx
                + *(metrics_os_it + *OS_it)
                )].push_back(new_shadow);
          }

          //Increment iterators
          ++NS_it;
          ++OS_it;
        }
      } while(curr_shadow.time_idx != K && (SK == -1 || curr_shadow.state_idx == SK));

      //***TRACEBACK***//
      new_node.prev_input = curr_shadow.prev_input;
      new_node.prev_state_idx = curr_shadow.prev_state_idx;
      expanded_it = d_real_nodes.begin() + (K-1)*S; //Place expanded_it at the last time index
      for(unsigned char* out_k=out + K-1 ; out_k >= out ; --out_k) {
        *out_k = (unsigned char)new_node.prev_input;
        new_node = *(expanded_it + new_node.prev_state_idx);

        expanded_it -= S;
      }

      //Clear expanded and shadow nodes containers
      for(size_t i=0 ; i<256 ; ++i) {
        d_shadow_nodes[i].clear();
      }

      for(std::vector<node>::iterator it=d_real_nodes.begin() ; it != d_real_nodes.end() ; ++it) {
        (*it).expanded=false;
      }
    }

  } /* namespace lazyviterbi */
} /* namespace gr */

