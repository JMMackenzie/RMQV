#include <iostream>
#include <unordered_map>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>

#include <succinct/mapper.hpp>


#include "index_types.hpp"
#include "wand_data_compressed.hpp"
#include "wand_data_raw.hpp"
#include "queries.hpp"
#include "util.hpp"
#include "queries_util.hpp"
#include "benchmark.h"

namespace {
void printUsage(const std::string &programName) {
  std::cerr << "Usage: " << programName
            << " index_type query_algorithm index_filename [--wand wand_data_filename]"
            << " [--compressed-wand] [--query query_filename] [--lexicon lexicon_file] [--k no_docs]" << std::endl;
}
} // namespace

using namespace ds2i;

template<typename Functor>
void op_cycle_count(Functor query_func, // XXX!!!
                 std::vector<std::pair<uint32_t, ds2i::term_id_vec>> const &queries) {
    using namespace ds2i;

        for (auto const &query: queries) {
            BEST_TIME_NOCHECK(query_func(query.second), , 5, false, query.first); 
        }

}


template<typename Functor>
void op_perftest(Functor query_func, // XXX!!!
                 std::vector<std::pair<uint32_t, ds2i::term_id_vec>> const &queries,
                 size_t runs) {
    using namespace ds2i;

    std::map<uint32_t, double> query_times;
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> profiled;

    for (size_t run = 0; run <= runs; ++run) {
        for (auto const &query: queries) {
            
            auto tick = get_time_usecs();
            std::pair<uint64_t, uint64_t> result = query_func(query.second);
            do_not_optimize_away(result);
            
            double elapsed = double(get_time_usecs() - tick);
            if (run != 0) { // first run is not timed
                auto itr = query_times.find(query.first);
                if(itr != query_times.end()) {
                    itr->second += elapsed;
                } else {
                    query_times[query.first] = elapsed;
                }
            }
            else {
              profiled[query.first] = result;
            }
        }
    }

    // Take mean of the timings and dump per-query
    for(auto& timing : query_times) {
      timing.second = timing.second / runs;
      auto profp = profiled[timing.first];
      std::cout << timing.first << ";" << (timing.second / 1000.0) <<  ";" << profp.first << ";" << profp.second << std::endl;
    }

}

template<typename IndexType, typename WandType>
void perftest(const char *index_filename,
              const char *wand_data_filename,
              std::vector<std::pair<uint32_t, ds2i::term_id_vec>> const &queries,
              std::string const &type,
              std::string const &query_type,
              const uint64_t m_k = 0) {
    using namespace ds2i;
    IndexType index;
    logger() << "Loading index from " << index_filename << std::endl;
    boost::iostreams::mapped_file_source m(index_filename);
    succinct::mapper::map(index, m);


/*
    logger() << "Warming up posting lists" << std::endl;
    std::unordered_set<term_id_type> warmed_up;
    for (auto const &q: queries) {
        for (auto t: q.second) {
            if (!warmed_up.count(t)) {
                index.warmup(t);
                warmed_up.insert(t);
            }
        }
    }*/

    WandType wdata;

    std::vector<std::string> query_types;
    boost::algorithm::split(query_types, query_type, boost::is_any_of(":"));
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
/*
    std::cerr << "Joel sanity test ranker:\n" 
              << "Av doclen = "
              << ranker->average_doc_len
              << std::endl
              << "numdocs = " << ranker->num_docs
              << std::endl
              << "total terms |D| = " << ranker->total_terms_in_collection << std::endl;
*/

  
    // Get the desired k
    uint64_t k = m_k;
    if (k == 0) {
      k = configuration::get().k;
    }

    logger() << "Performing " << type << " queries" << std::endl;
    for (auto const &t: query_types) {
        logger() << "Query type: " << t << std::endl;
        std::function<std::pair<uint64_t,uint64_t>(ds2i::term_id_vec)> query_fun;
        if (t == "and") {
            continue;
 //           query_fun = [&](ds2i::term_id_vec query) { return and_query<false>()(index, query); };
/*        } else if (t == "and_freq") {
            query_fun = [&](ds2i::term_id_vec query) { return and_query<true>()(index, query); };
        } else if (t == "or") {
            query_fun = [&](ds2i::term_id_vec query) { return or_query<false>()(index, query); };
        } else if (t == "or_freq") {
            query_fun = [&](ds2i::term_id_vec query) { return or_query<true>()(index, query); };
 */       } else if (t == "wand" && wand_data_filename) {
            query_fun = [&](ds2i::term_id_vec query) { return wand_query<WandType>(wdata, k)(index, query, ranker); };
        } else if (t == "block_max_wand" && wand_data_filename) {
            query_fun = [&](ds2i::term_id_vec query) { return block_max_wand_query<WandType>(wdata, k)(index, query, ranker); };
        } else if (t == "ranked_or" && wand_data_filename) {
            query_fun = [&](ds2i::term_id_vec query) { return ranked_or_query<WandType>(wdata, k)(index, query, ranker); };
        } else if (t == "maxscore" && wand_data_filename) {
            query_fun = [&](ds2i::term_id_vec query) { return maxscore_query<WandType>(wdata, k)(index, query, ranker); };
        } else {
            logger() << "Unsupported query type: " << t << std::endl;
            break;
        }
        #ifdef PROFILE
            op_cycle_count(query_fun, queries);
        #endif
        #ifndef PROFILE
            op_perftest(query_fun, queries, 4);
        #endif
    }


}

typedef wand_data<wand_data_raw> wand_raw_index;
typedef wand_data<wand_data_compressed<uniform_score_compressor>> wand_uniform_index;

int main(int argc, const char **argv) {
    using namespace ds2i;

    std::string programName = argv[0];
    if (argc < 3) {
    printUsage(programName);
    return 1;
    }

    std::string type = argv[1];
    std::string query_type = argv[2];
    const char *index_filename = argv[3];
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

    std::unordered_map<std::string, uint32_t> lexicon;
    if (lexicon_filename != nullptr) {
      std::ifstream in_lex(lexicon_filename);
      if (in_lex.is_open()) {
        read_lexicon(in_lex, lexicon);
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
#define LOOP_BODY(R, DATA, T)                                                            \
        } else if (type == BOOST_PP_STRINGIZE(T)) {                                      \
            if (compressed) {                                                            \
                 perftest<BOOST_PP_CAT(T, _index), wand_uniform_index>                   \
                 (index_filename, wand_data_filename, queries, type, query_type, m_k);   \
            } else {                                                                     \
                perftest<BOOST_PP_CAT(T, _index), wand_raw_index>                        \
                (index_filename, wand_data_filename, queries, type, query_type, m_k);    \
            }                                                                            \
    /**/

BOOST_PP_SEQ_FOR_EACH(LOOP_BODY, _, DS2I_INDEX_TYPES);
#undef LOOP_BODY

    } else {
        logger() << "ERROR: Unknown type " << type << std::endl;
    }

}
