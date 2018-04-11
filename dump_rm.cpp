#include <iostream>
#include <functional>
#include <unordered_map>
#include <thread>
#include <mutex>

#include <boost/unordered_map.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>

#include <succinct/mapper.hpp>


#include "index_types.hpp"
#include "wand_data_compressed.hpp"
#include "wand_data_raw.hpp"
#include "queries.hpp" // BOW queries
#include "weighted_queries.hpp" // RM queries
#include "util.hpp"
#include "docvector/document_index.hpp"

namespace {
void printUsage(const std::string &programName) {
  std::cerr << "Usage: " << programName
            << " index_type index_filename forward_index_filename --wand wand_data_filename"
            << " [--compressed-wand] [--query query_filename] [--k no_docs_for_expansion]"
            << " [--lexicon lexicon_file]" << std::endl;
}
} // namespace

using namespace ds2i;

typedef std::vector<std::pair<double, uint64_t>> top_k_list;

void do_join(std::thread& t)
{
    t.join();
}

template<typename IndexType, typename WandType>
void dump_rm(const char *index_filename,
             const char *wand_data_filename,
             const char *forward_index_filename,
             std::vector<std::pair<uint32_t, ds2i::term_id_vec>> const &queries,
             const uint64_t m_k,
             std::unordered_map<uint32_t, std::string>& reverse_lexicon) {

    using namespace ds2i;
    IndexType index;
    logger() << "Loading index from " << index_filename << std::endl;
    boost::iostreams::mapped_file_source m(index_filename);
    succinct::mapper::map(index, m);

    document_index forward_index;
    logger() << "Loading forward index from " << forward_index_filename << std::endl;
    forward_index.load(std::string(forward_index_filename));

    logger() << "Warming up posting lists" << std::endl;
    std::unordered_set<term_id_type> warmed_up;
    for (auto const &q: queries) {
        for (auto t: q.second) {
            if (!warmed_up.count(t)) {
                index.warmup(t);
                warmed_up.insert(t);
            }
        }
    }

    WandType wdata;
    boost::iostreams::mapped_file_source md;
    // Read the wand data
    if (wand_data_filename) {
        md.open(wand_data_filename);
        succinct::mapper::map(wdata, md, succinct::mapper::map_flags::warmup);
    }

    // Init our ranker
    std::unique_ptr<doc_scorer> ranker = build_ranker(wdata.average_doclen(), 
                                                      wdata.num_docs(),
                                                      wdata.terms_in_collection(),
                                                      wdata.ranker_id());
        
    for (auto const &query: queries) {
        
        auto tmp = wand_query<WandType>(wdata, m_k);
        tmp(index, query.second, ranker); 
        auto tk = tmp.topk();
        auto weighted_query = forward_index.rm_expander(tk);
        for(size_t i = 0; i < weighted_query.size() && i < 500; i++) {
          std::cerr << query.first << " " << i+1 << " " << reverse_lexicon[weighted_query[i].first] << " " << weighted_query[i].second << std::endl;
        }
    }
}

typedef wand_data<wand_data_raw> wand_raw_index;
typedef wand_data<wand_data_compressed<uniform_score_compressor>> wand_uniform_index;

int main(int argc, const char **argv) {
    using namespace ds2i;

    std::string programName = argv[0];
    if (argc < 4) {
    printUsage(programName);
    return 1;
    }

    std::string type = argv[1];
    const char *index_filename = argv[2];
    const char *forward_filename = argv[3];
    const char *wand_data_filename = nullptr;
    const char *query_filename = nullptr;
    const char *lexicon_filename = nullptr;
    uint64_t m_k = 0;
    bool compressed = false;
    std::vector<std::pair<uint32_t, term_id_vec>> queries;

    for (int i = 4; i < argc; ++i) {
        std::string arg = argv[i];

        if(arg == "--wand"){
            wand_data_filename = argv[++i];;
        }

        if(arg == "--compressed-wand"){
            compressed = true;
        }

        if (arg == "--query") {
            query_filename = argv[++i];
        }

        if (arg == "--k") {
          m_k = std::stoull(argv[++i]);
        }
 
        if (arg == "--lexicon") {
          lexicon_filename = argv[++i];
        }
    }

    if (lexicon_filename == nullptr) {
      std::cerr << "ERROR: Must provide lexicon. Quitting."
                << std::endl;
      return EXIT_FAILURE;
    }


    std::unordered_map<std::string, uint32_t> lexicon;
    std::unordered_map<uint32_t, std::string> reverse_lexicon;
    if (lexicon_filename != nullptr) {
      std::ifstream in_lex(lexicon_filename);
      if (in_lex.is_open()) {
        read_lexicon(in_lex, lexicon, reverse_lexicon);
      }
      else {
        std::cerr << "ERROR: Could not open lexicon file." << std::endl;
      }
    }

    term_id_vec q;
    uint32_t qid;
    if(query_filename){
        std::filebuf fb;
        if (fb.open(query_filename, std::ios::in)) {
            std::istream is(&fb);
            if (lexicon_filename != nullptr) {
              while (read_query(q, qid, lexicon, is)) queries.emplace_back(qid, q);
            }
            else {
              while (read_query(q, qid, is)) queries.emplace_back(qid, q);
            }
        }
    } else {
        if (lexicon_filename != nullptr) {
          while (read_query(q, qid, lexicon)) queries.emplace_back(qid, q);
        }
        else {
          while (read_query(q, qid)) queries.emplace_back(qid, q);
        }
    }


    /**/
    if (false) {
#define LOOP_BODY(R, DATA, T)                                                       \
        } else if (type == BOOST_PP_STRINGIZE(T)) {                                 \
            if (compressed) {                                                       \
                 dump_rm<BOOST_PP_CAT(T, _index), wand_uniform_index>              \
                 (index_filename, wand_data_filename, forward_filename, queries, m_k, reverse_lexicon);   \
            } else {                                                                \
                dump_rm<BOOST_PP_CAT(T, _index), wand_raw_index>                   \
                (index_filename, wand_data_filename, forward_filename, queries, m_k, reverse_lexicon);    \
            }                                                                       \
    /**/

BOOST_PP_SEQ_FOR_EACH(LOOP_BODY, _, DS2I_INDEX_TYPES);
#undef LOOP_BODY

    } else {
        logger() << "ERROR: Unknown type " << type << std::endl;
    }

}


