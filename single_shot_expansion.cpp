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
#include "collection_config.hpp"

namespace {
void printUsage(const std::string &programName) {
  std::cerr << "Usage: " << programName
            << " index_type query_algorithm param_file --output out_file --query query_file" << std::endl;
}
} // namespace

using namespace ds2i;


template<typename Functor>
void op_dump_trec(Functor query_func, // XXX!!!
                 std::vector<std::pair<uint32_t, ds2i::term_id_vec>> const &queries,
                 std::vector<std::string>& id_map,
                 std::string const &query_type,
                 std::ofstream& output) {
    using namespace ds2i;
    
    std::map<uint32_t, double> query_times;

    size_t runs = 5;
    for (size_t r = 0; r < runs; ++r) {
        // Run queries
        for (auto const &query: queries) {
            std::vector<std::pair<double, uint64_t>> top_k;
            auto tick = get_time_usecs();
            top_k = query_func(query.second); // All stages
            auto tock = get_time_usecs();
            double elapsed = (tock-tick);
      
            if (r == 0) {
              output_trec(top_k, query.first, id_map, query_type, output);
            }
            else {
                auto itr = query_times.find(query.first);
                if(itr != query_times.end()) {
                    itr->second += elapsed;
                } else {
                    query_times[query.first] = elapsed;
                }
            }
        }
    }

    // Take mean of the timings and dump per-query
    for(auto& timing : query_times) {
        timing.second = timing.second / runs;
        std::cout << timing.first << "," << (timing.second / 1000.0) <<  std::endl;
    }


}

typedef std::vector<std::pair<double, uint64_t>> top_k_list;

template<typename IndexType, typename WandType>
void rm_three_expansion(
              const collection_config& conf,
              std::vector<std::pair<uint32_t, ds2i::term_id_vec>> const &queries,
              std::string const &type,
              std::string const &query_type,
              const char *output_filename) {

    using namespace ds2i;
    IndexType index;
    logger() << "Loading index from " << conf.m_invidx_file << std::endl;
    boost::iostreams::mapped_file_source m(conf.m_invidx_file);
    succinct::mapper::map(index, m);

    document_index forward_index;
    logger() << "Loading forward index from " << conf.m_fidx_file << std::endl;
    forward_index.load(conf.m_fidx_file);

    std::vector<std::string> doc_map;
    logger() << "Loading map file from " << conf.m_map_file << std::endl;
    std::ifstream map_in(conf.m_map_file);
    std::string t_docid;
    while (map_in >> t_docid) {
      doc_map.emplace_back(t_docid); 
    }
    logger() << "Loaded " << doc_map.size() << " DocID's" << std::endl; 
  
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
    const char* wand_data_filename = conf.m_wand_file.c_str();
    std::vector<std::string> query_types;
    boost::algorithm::split(query_types, query_type, boost::is_any_of(":"));
    boost::iostreams::mapped_file_source md;
    if (wand_data_filename) {
        md.open(wand_data_filename);
        succinct::mapper::map(wdata, md, succinct::mapper::map_flags::warmup);
    }

    std::ofstream output_handle(output_filename);

    // Init our ranker
    std::unique_ptr<doc_scorer> ranker = build_ranker(wdata.average_doclen(), 
                                                      wdata.num_docs(),
                                                      wdata.terms_in_collection(),
                                                      wdata.ranker_id());
    // Final k documents to retrieve
    uint64_t k_final = conf.m_final_k;

    // Initial k docs to retrieve
    uint64_t k_expand = conf.m_docs_to_expand;    

    // Terms to expand
    uint64_t expand_term_count = conf.m_terms_to_expand;

    // Initial term weight
    double r_weight = conf.m_lambda;

    logger() << "Performing " << type << " queries" << std::endl;

    for (auto const &t: query_types) {
        logger() << "Query type: " << t << std::endl;
        
        std::function<std::vector<std::pair<double, uint64_t>>(ds2i::term_id_vec)> query_fun;
        if (t == "wand" && wand_data_filename) {
            query_fun = [&](ds2i::term_id_vec query) {
              // Default returns count of top-k, but we want the vector
              auto tmp = wand_query<WandType>(wdata, k_expand);
              tmp(index, query, ranker); 
              auto tk = tmp.topk();
              auto weighted_query = forward_index.rm_expander(tk, expand_term_count);
              normalize_weighted_query(weighted_query);
              add_original_query(r_weight, weighted_query, query);
              auto final_traversal = weighted_maxscore_query<WandType>(wdata, k_final);
              final_traversal(index, weighted_query, ranker);
              return final_traversal.topk();
            };
        } else if (t == "block_max_wand" && wand_data_filename) {
            query_fun = [&](ds2i::term_id_vec query) {
              auto tmp = block_max_wand_query<WandType>(wdata, k_expand);
              auto PROF = tmp(index, query, ranker);
              //std::cerr << "f_postings_scored," << PROF.second << std::endl;
 
              auto tk = tmp.topk();
              auto weighted_query = forward_index.rm_expander(tk, expand_term_count);
              normalize_weighted_query(weighted_query);
              add_original_query(r_weight, weighted_query, query);
              auto final_traversal = weighted_maxscore_query<WandType>(wdata, k_final);
              PROF = final_traversal(index, weighted_query, ranker);
              //std::cerr << "w_postings_scored," << PROF.second << std::endl;
 
              return final_traversal.topk();
            };
        }  else if (t == "ranked_or" && wand_data_filename) {
            query_fun = [&](ds2i::term_id_vec query) { 
              auto tmp = ranked_or_query<WandType>(wdata, k_expand);
              tmp(index, query, ranker);
              auto tk = tmp.topk();
              auto weighted_query = forward_index.rm_expander(tk, expand_term_count);
              normalize_weighted_query(weighted_query);
              add_original_query(r_weight, weighted_query, query);
              auto final_traversal = weighted_maxscore_query<WandType>(wdata, k_final);
              final_traversal(index, weighted_query, ranker);
              return final_traversal.topk();
          };
        } else if (t == "maxscore" && wand_data_filename) {
            query_fun = [&](ds2i::term_id_vec query) { 
              auto tmp = maxscore_query<WandType>(wdata, k_expand);
              tmp(index, query, ranker); 
              auto tk = tmp.topk();
              auto weighted_query = forward_index.rm_expander(tk, expand_term_count);
              normalize_weighted_query(weighted_query);
              add_original_query(r_weight, weighted_query, query);
              auto final_traversal = weighted_maxscore_query<WandType>(wdata, k_final);
              final_traversal(index, weighted_query, ranker);
              return final_traversal.topk();
            };
        } else {
            logger() << "Unsupported query type: " << t << std::endl;
            break;
        }

        op_dump_trec(query_fun, queries, doc_map, t, output_handle);
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

    document_index forward_index;

    std::string type = argv[1];
    std::string query_type = argv[2];
    std::string index_param = argv[3];
    const char *query_filename = nullptr;
    const char *out_filename = nullptr;
    bool compressed = false;
    std::vector<std::pair<uint32_t, term_id_vec>> queries;

    for (int i = 4; i < argc; ++i) {
        std::string arg = argv[i];

        if(arg == "--compressed-wand"){
            compressed = true;
        }

        if (arg == "--query") {
            query_filename = argv[++i];
        }

        if (arg == "--output") {
          out_filename = argv[++i];
        }
    }

    if (out_filename == nullptr) {
      std::cerr << "ERROR: Must provide output file. Quitting."
                << std::endl;
      return EXIT_FAILURE;
    }
    
    std::ifstream inconf(index_param);
    collection_config conf(inconf, true);

    std::unordered_map<std::string, uint32_t> lexicon;
    if (conf.m_lexicon_file != "") {
      std::ifstream in_lex(conf.m_lexicon_file);
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
            if (conf.m_lexicon_file != "") {
              while (read_query(q, qid, lexicon, is)) queries.emplace_back(qid, q);
            }
            else {
              while (read_query(q, qid, is)) queries.emplace_back(qid, q);
            }
        }
    } else {
        if (conf.m_lexicon_file != "") {
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
                 rm_three_expansion<BOOST_PP_CAT(T, _index), wand_uniform_index>              \
                 (conf, queries, type, query_type, out_filename);   \
            } else {                                                                \
                rm_three_expansion<BOOST_PP_CAT(T, _index), wand_raw_index>                   \
                (conf, queries, type, query_type, out_filename);    \
            }                                                                       \
    /**/

BOOST_PP_SEQ_FOR_EACH(LOOP_BODY, _, DS2I_INDEX_TYPES);
#undef LOOP_BODY

    } else {
        logger() << "ERROR: Unknown type " << type << std::endl;
    }

}
