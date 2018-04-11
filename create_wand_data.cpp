#include <fstream>
#include <iostream>

#include "binary_collection.hpp"
#include "binary_freq_collection.hpp"
#include "succinct/mapper.hpp"
#include "util.hpp"
#include "wand_data.hpp"
#include "wand_data_compressed.hpp"
#include "wand_data_raw.hpp"

namespace {
void printUsage(const std::string &programName) {
  std::cerr << "Usage: " << programName
            << " <collection basename> <output filename> <ranker name>"
            << " [--variable-block]"
            << "[--compress]" << std::endl;
  std::cerr << "Ranker names are: BM25 or LMDS" << std::endl;
}
} // namespace

void joel_warn() {
  std::cerr << "JOELS WARNING: Check your configuration is correct."
            << std::endl 
            << "configuration.hpp --> DS2I_BLOCK_SIZE for fixed block size,"
            << std::endl
            << "                  --> DS2I_FIXED_COST_WAND_PARTITION needs to be binary searched to find the appropriate *average* block-size."
            << std::endl
            << "                  --> DS2I_SCORE_REFERENCES_SIZE is the quantization granularity for CVBMW"
            << std::endl;
}

int main(int argc, const char **argv) {
  using namespace ds2i;
  std::string programName = argv[0];
  if (argc < 3) {
    printUsage(programName);
    return 1;
  }

  joel_warn();

  std::string input_basename = argv[1];
  const char *output_filename = argv[2];
  const char *ranker_name = argv[3];
  partition_type p_type = partition_type::fixed_blocks;
  bool compress = false;

  for (int i = 4; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--variable-block") {
      p_type = partition_type::variable_blocks;
    } else if (arg == "--compress") {
      compress = true;
    } else {
      printUsage(programName);
      return 1;
    }
  }

  std::string partition_type_name = (p_type == partition_type::fixed_blocks)
                                        ? "static partition"
                                        : "variable partition";
  logger() << "Block based wand creation with " << partition_type_name
           << std::endl;

  binary_collection sizes_coll((input_basename + ".sizes").c_str());
  binary_freq_collection coll(input_basename.c_str());

  // Build ranker
  ranker_identifier ranker_id = get_ranker_id(ranker_name); 
  std::unique_ptr<doc_scorer> ranker = build_ranker(ranker_id);

  if (compress) {

    if (ranker->id() == ranker_identifier::LMDS) {
        std::cerr << "Cannot use compression for block-max document weights. Implement me, or go without."
                  << std::endl;
        return 1;
    }

    wand_data<wand_data_compressed<uniform_score_compressor>> wdata(
        sizes_coll.begin()->begin(), coll.num_docs(), coll, p_type, ranker);
    succinct::mapper::freeze(wdata, output_filename);
  } else {
    wand_data<wand_data_raw> wdata(sizes_coll.begin()->begin(),
                                               coll.num_docs(), coll, p_type, ranker);
    succinct::mapper::freeze(wdata, output_filename);
  }
}
