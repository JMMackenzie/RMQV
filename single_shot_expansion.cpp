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
            << " index_type query_algorithm index_filename forward_index_filename --map map_filename --output out_name --wand wand_data_filename"
            << " [--compressed-wand] [--query query_filename] [--kexp no_docs_for_expansion] [--texp no_terms_to_expand]
            << " [--rweight rm_weight_original_query [0, 1]] [--kfinal no_docs_for_final] [--lexicon lexicon_file] " << std::endl;
}
} // namespace

using namespace ds2i;

void output_trec(std::vector<std::pair<double, uint64_t>>& top_k,
        uint32_t topic_id,
        std::vector<std::string>& id_map,
        std::string const &query_type,
        std::ofstream& output) {
    for (size_t n = 0; n < top_k.size(); ++n) {
        output << topic_id << " "
            << "Q0" << " "
            << id_map[top_k[n].second] << " "
            << n+1 << " "
            << top_k[n].first << " "
            << query_type << std::endl; 
    }
}

template<typename Functor>
void op_dump_trec(Functor query_func, // XXX!!!
                 std::vector<std::pair<uint32_t, ds2i::term_id_vec>> const &queries,
                 std::vector<std::string>& id_map,
                 std::string const &query_type,
                 std::ofstream& output) {
    using namespace ds2i;
    
    // Run queries
    for (auto const &query: queries) {
      std::vector<std::pair<double, uint64_t>> top_k;
      top_k = query_func(query.second); // All stages
      output_trec(top_k, query.first, id_map, query_type, output); 
    }
}

typedef boost::unordered_map<uint64_t, double> fuse_map;
typedef std::vector<std::pair<double, uint64_t>> top_k_list;

void do_join(std::thread& t)
{
    t.join();
}

template<typename IndexType, typename WandType>
void rm_three_expansion(const char *index_filename,
              const char *wand_data_filename,
              const char *forward_index_filename,
              std::vector<std::pair<uint32_t, ds2i::term_id_vec>> const &queries,
              std::string const &type,
              std::string const &query_type,
              const char *map_filename,
              const char *output_filename,
              const uint64_t m_k,
              const uint64_t exp_k,
              const uint64_t expand_term_count,
              const double r_weight) {
    using namespace ds2i;
    IndexType index;
    logger() << "Loading index from " << index_filename << std::endl;
    boost::iostreams::mapped_file_source m(index_filename);
    succinct::mapper::map(index, m);

    document_index forward_index;
    logger() << "Loading forward index from " << forward_index_filename << std::endl;
    forward_index.load(std::string(forward_index_filename));

    std::vector<std::string> doc_map;
    logger() << "Loading map file from " << map_filename << std::endl;
    std::ifstream map_in(map_filename);
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
    uint64_t k_final = m_k;

    // Initial k docs to retrieve
    uint64_t k_expand = exp_k;    

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
              auto final_traversal = weighted_maxscore_query<WandType>(wdata, k_final);
              final_traversal(index, weighted_query, ranker);
              return final_traversal.topk();
            };
        } else if (t == "block_max_wand" && wand_data_filename) {
            query_fun = [&](ds2i::term_id_vec query) {
              auto tmp = block_max_wand_query<WandType>(wdata, k_expand);
              tmp(index, query, ranker);
              auto tk = tmp.topk();
              auto weighted_query = forward_index.rm_expander(tk, expand_term_count);
              auto final_traversal = weighted_maxscore_query<WandType>(wdata, k_final);
              final_traversal(index, weighted_query, ranker);
              return final_traversal.topk();
            };
        }  else if (t == "ranked_or" && wand_data_filename) {
            query_fun = [&](ds2i::term_id_vec query) { 
              auto tmp = ranked_or_query<WandType>(wdata, k_expand);
              tmp(index, query, ranker);
              auto tk = tmp.topk();
              auto weighted_query = forward_index.rm_expander(tk, expand_term_count);
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
    const char *index_filename = argv[3];
    const char *forward_filename = argv[4];
    const char *wand_data_filename = nullptr;
    const char *query_filename = nullptr;
    const char *map_filename = nullptr;
    const char *out_filename = nullptr;
    const char *lexicon_filename = nullptr;
    uint64_t m_k = 10;
    uint64_t exp_k = 10;
    uint64_t exp_t = 10;
    double r_weight = 0.5;
    bool compressed = false;
    std::vector<std::pair<uint32_t, term_id_vec>> queries;

    for (int i = 5; i < argc; ++i) {
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

        if (arg == "--map") {
            map_filename = argv[++i]; 
        }

        if (arg == "--output") {
          out_filename = argv[++i];
        }

        if (arg == "--kfinal") {
          m_k = std::stoull(argv[++i]);
        }

        if (arg == "--kexp") {
          exp_k = std::stoull(argv[++i]);
        }

        if (arg == "--texp") {
          exp_t = std::stoull(argv[++i]);
        }
 
        if (arg == "--lexicon") {
          lexicon_filename = argv[++i];
        }

        if (arg == "--rweight") {
          r_weight = std::stod(argv[++i]);
        }
    }

    if (out_filename == nullptr || map_filename == nullptr) {
      std::cerr << "ERROR: Must provide map and output file. Quitting."
                << std::endl;
      return EXIT_FAILURE;
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
#define LOOP_BODY(R, DATA, T)                                                       \
        } else if (type == BOOST_PP_STRINGIZE(T)) {                                 \
            if (compressed) {                                                       \
                 rm_three_expansion<BOOST_PP_CAT(T, _index), wand_uniform_index>              \
                 (index_filename, wand_data_filename, forward_filename, queries, type, query_type, map_filename, out_filename, m_k, exp_k, exp_t, r_weight);   \
            } else {                                                                \
                rm_three_expansion<BOOST_PP_CAT(T, _index), wand_raw_index>                   \
                (index_filename, wand_data_filename, forward_filename, queries, type, query_type, map_filename, out_filename, m_k, exp_k, exp_t, r_weight);    \
            }                                                                       \
    /**/

BOOST_PP_SEQ_FOR_EACH(LOOP_BODY, _, DS2I_INDEX_TYPES);
#undef LOOP_BODY

    } else {
        logger() << "ERROR: Unknown type " << type << std::endl;
    }

}
