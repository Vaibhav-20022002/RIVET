#include "rivet/cli/Cli.hpp"
#include "rivet/config/RivetConfig.hpp"
#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>

using namespace rivet::config;
using namespace rivet::cli;

/// ============================================================================
/// RivetConfig Tests
/// ============================================================================

TEST(RivetConfigTest, DefaultValues) {
  RivetConfig cfg;
  EXPECT_EQ(cfg.rtsp_port, 8554u);
  EXPECT_EQ(cfg.stream_name, "live/test");
  EXPECT_EQ(cfg.egress_shards, 2u);
  EXPECT_EQ(cfg.base_rtp_port, 20000u);
  EXPECT_EQ(cfg.base_rtcp_port, 20001u);
  EXPECT_EQ(cfg.watchdog_timeout_ms, 5000u);
  EXPECT_EQ(cfg.numa_capacity, 4096u);
  EXPECT_EQ(cfg.numa_slab_size, 2048u);
  EXPECT_EQ(cfg.startup_policy, "FastStartup");
  EXPECT_EQ(cfg.drop_policy, "DependencyAware");
  EXPECT_EQ(cfg.log_level, "INFO");
}

TEST(RivetConfigTest, LoadDotenvFile) {
  std::string tmp_env = "test_config_tmp.env";
  {
    std::ofstream out(tmp_env);
    out << "# Test comment\n";
    out << "RIVET_TEST_KEY_1=hello\n";
    out << "RIVET_TEST_KEY_2=\"world with spaces\"\n";
    out << "  RIVET_TEST_KEY_3 = 'single quotes'  \n";
  }

  EXPECT_TRUE(RivetConfig::load_dotenv(tmp_env));
  EXPECT_STREQ(std::getenv("RIVET_TEST_KEY_1"), "hello");
  EXPECT_STREQ(std::getenv("RIVET_TEST_KEY_2"), "world with spaces");
  EXPECT_STREQ(std::getenv("RIVET_TEST_KEY_3"), "single quotes");

  std::remove(tmp_env.c_str());
}

TEST(RivetConfigTest, PrintDashboardRunsSafely) {
  RivetConfig cfg;
  testing::internal::CaptureStdout();
  cfg.print_dashboard();
  std::string out = testing::internal::GetCapturedStdout();
  EXPECT_TRUE(out.find("RIVET System Settings") != std::string::npos);
  EXPECT_TRUE(out.find("8554") != std::string::npos);
}

/// ============================================================================
/// Cli Tests
/// ============================================================================

TEST(CliTest, HelpFlag) {
  char  prog[] = "rivet";
  char  arg1[] = "-h";
  char *argv[] = {prog, arg1, nullptr};

  RivetConfig cfg;
  std::string env_path;

  testing::internal::CaptureStdout();
  auto        res = parse_args(2, argv, cfg, env_path);
  std::string out = testing::internal::GetCapturedStdout();

  EXPECT_EQ(res, ParseResult::HelpRequested);
  EXPECT_TRUE(out.find("Usage:") != std::string::npos);
}

TEST(CliTest, PortAndStreamOverrides) {
  char prog[]      = "rivet";
  char p_flag[]    = "-p";
  char p_val[]     = "9554";
  char s_flag[]    = "-s";
  char s_val[]     = "camera/front";
  char n_flag[]    = "-n";
  char n_val[]     = "4";
  char rtp_flag[]  = "--rtp-port";
  char rtp_val[]   = "30000";
  char rtcp_flag[] = "--rtcp-port";
  char rtcp_val[]  = "30001";

  char *argv[] = {prog,
      p_flag,
      p_val,
      s_flag,
      s_val,
      n_flag,
      n_val,
      rtp_flag,
      rtp_val,
      rtcp_flag,
      rtcp_val,
      nullptr};

  RivetConfig cfg;
  std::string env_path;

  auto res = parse_args(11, argv, cfg, env_path);
  EXPECT_EQ(res, ParseResult::Success);
  EXPECT_EQ(cfg.rtsp_port, 9554u);
  EXPECT_EQ(cfg.stream_name, "camera/front");
  EXPECT_EQ(cfg.egress_shards, 4u);
  EXPECT_EQ(cfg.base_rtp_port, 30000u);
  EXPECT_EQ(cfg.base_rtcp_port, 30001u);
}

TEST(CliTest, UnknownFlagReturnsError) {
  char  prog[] = "rivet";
  char  bad[]  = "--invalid-option";
  char *argv[] = {prog, bad, nullptr};

  RivetConfig cfg;
  std::string env_path;

  testing::internal::CaptureStderr();
  auto res = parse_args(2, argv, cfg, env_path);
  testing::internal::GetCapturedStderr();

  EXPECT_EQ(res, ParseResult::Error);
}
