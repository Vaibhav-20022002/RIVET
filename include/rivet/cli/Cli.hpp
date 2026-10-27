#pragma once

#include "rivet/config/RivetConfig.hpp"
#include <string>

namespace rivet::cli {

enum class ParseResult { Success, HelpRequested, Error };

/// Prints formatted CLI usage to standard output
void print_usage(const char *prog_name);

/// Parses command line arguments, loading env configuration and applying overrides
ParseResult parse_args(int argc, char **argv, config::RivetConfig &config, std::string &env_path);

/// Main execution entry point for the RIVET server process
int run(int argc, char **argv);

} //< namespace rivet::cli
