/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2020 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "sched_test_common.h"
#include "srsenb/hdr/stack/mac/sched.h"
#include "srslte/adt/accumulators.h"
#include <chrono>

namespace srsenb {

struct run_params {
  uint32_t    nof_prbs;
  uint32_t    nof_ues;
  uint32_t    nof_ttis;
  uint32_t    cqi;
  const char* sched_policy;
};

struct run_params_range {
  std::vector<uint32_t>    nof_prbs     = {6, 15, 25, 50, 75, 100};
  std::vector<uint32_t>    nof_ues      = {1, 2, 5};
  uint32_t                 nof_ttis     = 10000;
  std::vector<uint32_t>    cqi          = {5, 10, 15};
  std::vector<const char*> sched_policy = {"time_rr", "time_pf"};

  size_t     nof_runs() const { return nof_prbs.size() * nof_ues.size() * cqi.size() * sched_policy.size(); }
  run_params get_params(size_t idx) const
  {
    run_params r = {};
    r.nof_ttis   = nof_ttis;
    r.nof_prbs   = nof_prbs[idx % nof_prbs.size()];
    idx /= nof_prbs.size();
    r.nof_ues = nof_ues[idx % nof_ues.size()];
    idx /= nof_ues.size();
    r.cqi = cqi[idx % cqi.size()];
    idx /= cqi.size();
    r.sched_policy = sched_policy.at(idx);
    return r;
  }
};

class sched_tester : public sched_sim_base
{
  static std::vector<sched_interface::cell_cfg_t> get_cell_cfg(srslte::span<const sched_cell_params_t> cell_params)
  {
    std::vector<sched_interface::cell_cfg_t> cell_cfg_list;
    for (auto& c : cell_params) {
      cell_cfg_list.push_back(c.cfg);
    }
    return cell_cfg_list;
  }

public:
  explicit sched_tester(sched*                                          sched_obj_,
                        const sched_interface::sched_args_t&            sched_args,
                        const std::vector<sched_interface::cell_cfg_t>& cell_cfg_list) :
    sched_sim_base(sched_obj_, sched_args, cell_cfg_list),
    sched_ptr(sched_obj_),
    dl_result(cell_cfg_list.size()),
    ul_result(cell_cfg_list.size())
  {}

  srslog::basic_logger& mac_logger = srslog::fetch_basic_logger("MAC");
  sched*                sched_ptr;
  uint32_t              dl_bytes_per_tti   = 100000;
  uint32_t              ul_bytes_per_tti   = 100000;
  run_params            current_run_params = {};

  std::vector<sched_interface::dl_sched_res_t> dl_result;
  std::vector<sched_interface::ul_sched_res_t> ul_result;

  struct throughput_stats {
    srslte::rolling_average<float> mean_dl_tbs, mean_ul_tbs, avg_dl_mcs, avg_ul_mcs;
    srslte::rolling_average<float> avg_latency;
  };
  throughput_stats total_stats;

  int advance_tti()
  {
    tti_point tti_rx = get_tti_rx().is_valid() ? get_tti_rx() + 1 : tti_point(0);
    mac_logger.set_context(tti_rx.to_uint());
    new_tti(tti_rx);

    for (uint32_t cc = 0; cc < get_cell_params().size(); ++cc) {
      std::chrono::time_point<std::chrono::steady_clock> tp = std::chrono::steady_clock::now();
      TESTASSERT(sched_ptr->dl_sched(to_tx_dl(tti_rx).to_uint(), cc, dl_result[cc]) == SRSLTE_SUCCESS);
      TESTASSERT(sched_ptr->ul_sched(to_tx_ul(tti_rx).to_uint(), cc, ul_result[cc]) == SRSLTE_SUCCESS);
      std::chrono::time_point<std::chrono::steady_clock> tp2 = std::chrono::steady_clock::now();
      std::chrono::nanoseconds tdur = std::chrono::duration_cast<std::chrono::nanoseconds>(tp2 - tp);
      total_stats.avg_latency.push(tdur.count());
    }

    sf_output_res_t sf_out{get_cell_params(), tti_rx, ul_result, dl_result};
    update(sf_out);
    process_stats(sf_out);

    return SRSLTE_SUCCESS;
  }

  void set_external_tti_events(const sim_ue_ctxt_t& ue_ctxt, ue_tti_events& pending_events) override
  {
    // do nothing
    if (ue_ctxt.conres_rx) {
      sched_ptr->ul_bsr(ue_ctxt.rnti, 1, dl_bytes_per_tti);
      sched_ptr->dl_rlc_buffer_state(ue_ctxt.rnti, 3, ul_bytes_per_tti, 0);

      if (get_tti_rx().to_uint() % 5 == 0) {
        for (uint32_t cc = 0; cc < pending_events.cc_list.size(); ++cc) {
          pending_events.cc_list[cc].dl_cqi = current_run_params.cqi;
          pending_events.cc_list[cc].ul_snr = 40;
        }
      }
    }
  }

  void process_stats(sf_output_res_t& sf_out)
  {
    for (uint32_t cc = 0; cc < get_cell_params().size(); ++cc) {
      uint32_t dl_tbs = 0, ul_tbs = 0, dl_mcs = 0, ul_mcs = 0;
      for (uint32_t i = 0; i < sf_out.dl_cc_result[cc].nof_data_elems; ++i) {
        dl_tbs += sf_out.dl_cc_result[cc].data[i].tbs[0];
        dl_tbs += sf_out.dl_cc_result[cc].data[i].tbs[1];
        dl_mcs = std::max(dl_mcs, sf_out.dl_cc_result[cc].data[i].dci.tb[0].mcs_idx);
      }
      total_stats.mean_dl_tbs.push(dl_tbs);
      if (sf_out.dl_cc_result[cc].nof_data_elems > 0) {
        total_stats.avg_dl_mcs.push(dl_mcs);
      }
      for (uint32_t i = 0; i < sf_out.ul_cc_result[cc].nof_dci_elems; ++i) {
        ul_tbs += sf_out.ul_cc_result[cc].pusch[i].tbs;
        ul_mcs = std::max(ul_mcs, sf_out.ul_cc_result[cc].pusch[i].dci.tb.mcs_idx);
      }
      total_stats.mean_ul_tbs.push(ul_tbs);
      if (sf_out.ul_cc_result[cc].nof_dci_elems) {
        total_stats.avg_ul_mcs.push(ul_mcs);
      }
    }
  }
};

int run_sched_new_ue(sched_tester&                    sched_tester,
                     const run_params&                params,
                     uint16_t                         rnti,
                     const sched_interface::ue_cfg_t& ue_cfg)
{
  const uint32_t ENB_CC_IDX = 0;

  sched_tester.total_stats        = {};
  sched_tester.current_run_params = params;

  // Add user (first need to advance to a PRACH TTI)
  while (not srslte_prach_tti_opportunity_config_fdd(
      sched_tester.get_cell_params()[ue_cfg.supported_cc_list[0].enb_cc_idx].cfg.prach_config,
      sched_tester.get_tti_rx().to_uint(),
      -1)) {
    TESTASSERT(sched_tester.advance_tti() == SRSLTE_SUCCESS);
  }
  TESTASSERT(sched_tester.add_user(rnti, ue_cfg, 16) == SRSLTE_SUCCESS);

  // Ignore stats of the first TTIs until UE DRB1 is added
  while (not sched_tester.get_enb_ctxt().ue_db.at(rnti)->conres_rx) {
    sched_tester.advance_tti();
  }
  sched_tester.total_stats = {};

  for (uint32_t count = 0; count < params.nof_ttis; ++count) {
    sched_tester.advance_tti();
  }

  return SRSLTE_SUCCESS;
}

struct run_data {
  run_params                params;
  float                     avg_dl_throughput;
  float                     avg_ul_throughput;
  float                     avg_dl_mcs;
  float                     avg_ul_mcs;
  std::chrono::microseconds avg_latency;
};

int run_benchmark_scenario(run_params params, std::vector<run_data>& run_results)
{
  std::vector<sched_interface::cell_cfg_t> cell_list(1, generate_default_cell_cfg(params.nof_prbs));
  sched_interface::ue_cfg_t                ue_cfg_default = generate_default_ue_cfg();
  sched_interface::sched_args_t            sched_args     = {};
  sched_args.sched_policy                                 = params.sched_policy;

  sched     sched_obj;
  rrc_dummy rrc{};
  sched_obj.init(&rrc, sched_args);
  sched_tester tester(&sched_obj, sched_args, cell_list);

  tester.total_stats        = {};
  tester.current_run_params = params;

  for (uint32_t ue_idx = 0; ue_idx < params.nof_ues; ++ue_idx) {
    uint16_t rnti = 0x46 + ue_idx;
    // Add user (first need to advance to a PRACH TTI)
    while (not srslte_prach_tti_opportunity_config_fdd(
        tester.get_cell_params()[ue_cfg_default.supported_cc_list[0].enb_cc_idx].cfg.prach_config,
        tester.get_tti_rx().to_uint(),
        -1)) {
      TESTASSERT(tester.advance_tti() == SRSLTE_SUCCESS);
    }
    TESTASSERT(tester.add_user(rnti, ue_cfg_default, 16) == SRSLTE_SUCCESS);
    TESTASSERT(tester.advance_tti() == SRSLTE_SUCCESS);
  }

  // Ignore stats of the first TTIs until all UEs DRB1 are created
  auto ue_db_ctxt = tester.get_enb_ctxt().ue_db;
  while (not std::all_of(ue_db_ctxt.begin(), ue_db_ctxt.end(), [](std::pair<uint16_t, const sim_ue_ctxt_t*> p) {
    return p.second->conres_rx;
  })) {
    tester.advance_tti();
    ue_db_ctxt = tester.get_enb_ctxt().ue_db;
  }
  tester.total_stats = {};

  // Run benchmark
  for (uint32_t count = 0; count < params.nof_ttis; ++count) {
    tester.advance_tti();
  }

  run_data run_result          = {};
  run_result.params            = params;
  run_result.avg_dl_throughput = tester.total_stats.mean_dl_tbs.value() * 8.0 / 1e-3;
  run_result.avg_ul_throughput = tester.total_stats.mean_ul_tbs.value() * 8.0 / 1e-3;
  run_result.avg_dl_mcs        = tester.total_stats.avg_dl_mcs.value();
  run_result.avg_ul_mcs        = tester.total_stats.avg_ul_mcs.value();
  run_result.avg_latency = std::chrono::microseconds(static_cast<int>(tester.total_stats.avg_latency.value() / 1000));
  run_results.push_back(run_result);

  return SRSLTE_SUCCESS;
}

run_data expected_run_result(run_params params)
{
  assert(params.cqi == 15 && "only cqi=15 supported for now");
  run_data ret{};
  int      tbs_idx      = srslte_ra_tbs_idx_from_mcs(28, false, false);
  int      tbs          = srslte_ra_tbs_from_idx(tbs_idx, params.nof_prbs);
  ret.avg_dl_throughput = tbs * 1e3; // bps

  tbs_idx                 = srslte_ra_tbs_idx_from_mcs(28, false, true);
  uint32_t nof_pusch_prbs = params.nof_prbs - (params.nof_prbs == 6 ? 2 : 4);
  tbs                     = srslte_ra_tbs_from_idx(tbs_idx, nof_pusch_prbs);
  ret.avg_ul_throughput   = tbs * 1e3; // bps

  ret.avg_dl_mcs = 27;
  ret.avg_ul_mcs = 22;
  switch (params.nof_prbs) {
    case 6:
      ret.avg_dl_mcs = 25;
      ret.avg_dl_throughput *= 0.7;
      ret.avg_ul_throughput *= 0.25;
      break;
    case 15:
      ret.avg_dl_throughput *= 0.95;
      ret.avg_ul_throughput *= 0.5;
      break;
    default:
      ret.avg_dl_throughput *= 0.97;
      ret.avg_ul_throughput *= 0.5;
      break;
  }
  return ret;
}

void print_benchmark_results(const std::vector<run_data>& run_results)
{
  srslog::flush();
  fmt::print("run | Nprb | cqi | sched pol | Nue | DL/UL [Mbps] | DL/UL mcs | DL/UL OH [%] | latency "
             "[usec]\n");
  fmt::print("---------------------------------------------------------------------------------------"
             "------\n");
  for (uint32_t i = 0; i < run_results.size(); ++i) {
    const run_data& r = run_results[i];

    int   tbs_idx           = srslte_ra_tbs_idx_from_mcs(28, false, false);
    int   tbs               = srslte_ra_tbs_from_idx(tbs_idx, r.params.nof_prbs);
    float dl_rate_overhead  = 1.0F - r.avg_dl_throughput / (tbs * 1e3);
    tbs_idx                 = srslte_ra_tbs_idx_from_mcs(28, false, true);
    uint32_t nof_pusch_prbs = r.params.nof_prbs - (r.params.nof_prbs == 6 ? 2 : 4);
    tbs                     = srslte_ra_tbs_from_idx(tbs_idx, nof_pusch_prbs);
    float ul_rate_overhead  = 1.0F - r.avg_ul_throughput / (tbs * 1e3);

    fmt::print("{:>3d}{:>6d}{:>6d}{:>12}{:>6d}{:>9.2}/{:>4.2}{:>9.1f}/{:>4.1f}{:9.1f}/{:>4.1f}{:12d}\n",
               i,
               r.params.nof_prbs,
               r.params.cqi,
               r.params.sched_policy,
               r.params.nof_ues,
               r.avg_dl_throughput / 1e6,
               r.avg_ul_throughput / 1e6,
               r.avg_dl_mcs,
               r.avg_ul_mcs,
               dl_rate_overhead * 100,
               ul_rate_overhead * 100,
               r.avg_latency.count());
  }
}

int run_rate_test()
{
  fmt::print("\n====== Scheduler Rate Test ======\n\n");
  run_params_range      run_param_list{};
  srslog::basic_logger& mac_logger = srslog::fetch_basic_logger("MAC");

  run_param_list.nof_ues = {1};
  run_param_list.cqi     = {15};

  std::vector<run_data> run_results;
  size_t                nof_runs = run_param_list.nof_runs();

  for (size_t r = 0; r < nof_runs; ++r) {
    run_params runparams = run_param_list.get_params(r);

    mac_logger.info("\n=== New run {} ===\n", r);
    TESTASSERT(run_benchmark_scenario(runparams, run_results) == SRSLTE_SUCCESS);
  }

  print_benchmark_results(run_results);

  bool success = true;
  for (auto& run : run_results) {
    run_data expected = expected_run_result(run.params);
    if (run.avg_dl_mcs < expected.avg_dl_mcs) {
      fmt::print(
          "Nprb={:>2d}: DL mcs below expected ({} < {})\n", run.params.nof_prbs, run.avg_dl_mcs, expected.avg_dl_mcs);
      success = false;
    }
    if (run.avg_dl_throughput < expected.avg_dl_throughput) {
      fmt::print("Nprb={:>2d}: DL rate below expected ({:.2} < {:.2}) Mbps\n",
                 run.params.nof_prbs,
                 run.avg_dl_throughput / 1e6,
                 expected.avg_dl_throughput / 1e6);
      success = false;
    }
    if (run.avg_ul_mcs < expected.avg_ul_mcs) {
      fmt::print(
          "Nprb={:>2d}: UL mcs below expected ({} < {})\n", run.params.nof_prbs, run.avg_ul_mcs, expected.avg_ul_mcs);
      success = false;
    }
    if (run.avg_ul_throughput < expected.avg_ul_throughput) {
      fmt::print("Nprb={:>2d}: UL rate below expected ({:.2} < {:.2}) Mbps\n",
                 run.params.nof_prbs,
                 run.avg_ul_throughput / 1e6,
                 expected.avg_ul_throughput / 1e6);
      success = false;
    }
  }
  return success ? SRSLTE_SUCCESS : SRSLTE_ERROR;
}

int run_benchmark()
{
  run_params_range      run_param_list{};
  srslog::basic_logger& mac_logger = srslog::fetch_basic_logger("MAC");

  std::vector<run_data> run_results;
  size_t                nof_runs = run_param_list.nof_runs();
  for (size_t r = 0; r < nof_runs; ++r) {
    run_params runparams = run_param_list.get_params(r);

    mac_logger.info("\n### New run {} ###\n", r);
    TESTASSERT(run_benchmark_scenario(runparams, run_results) == SRSLTE_SUCCESS);
  }

  print_benchmark_results(run_results);

  return SRSLTE_SUCCESS;
}

} // namespace srsenb

int main()
{
  // Setup the log spy to intercept error and warning log entries.
  if (!srslog::install_custom_sink(
          srslte::log_sink_spy::name(),
          std::unique_ptr<srslte::log_sink_spy>(new srslte::log_sink_spy(srslog::get_default_log_formatter())))) {
    return SRSLTE_ERROR;
  }

  auto* spy = static_cast<srslte::log_sink_spy*>(srslog::find_sink(srslte::log_sink_spy::name()));
  if (!spy) {
    return SRSLTE_ERROR;
  }

  auto& mac_log = srslog::fetch_basic_logger("MAC");
  mac_log.set_level(srslog::basic_levels::warning);
  auto& test_log = srslog::fetch_basic_logger("TEST", *spy, false);
  test_log.set_level(srslog::basic_levels::warning);

  // Start the log backend.
  srslog::init();

  bool run_benchmark = false;

  TESTASSERT(srsenb::run_rate_test() == SRSLTE_SUCCESS);
  if (run_benchmark) {
    TESTASSERT(srsenb::run_benchmark() == SRSLTE_SUCCESS);
  }

  return 0;
}