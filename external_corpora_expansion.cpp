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
            << " index_type query_algorithm index_filename forward_index_filename ext_index_filename ext_forward_index_filename --map map_filename --ext_map ext_map_filename --output out_name --wand wand_data_filename --ext_wand ext_wand_data_filename"
            << " [--compressed-wand] --query query_filename --kexp no_docs_for_expansion --texp no_terms_to_expand"
            << " --rweight rm_weight_original_query [0, 1] --kfinal no_docs_for_final --lexicon lexicon_file " << std::endl;
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
    
    // Run queries
    for (auto const &query: queries) {
      std::vector<std::pair<double, uint64_t>> top_k;
      auto tick = get_time_usecs();
      top_k = query_func(query.second); // All stages
      auto tock = get_time_usecs();
      double elapsedms = (tock-tick)/1000;
      std::cerr << query.first << " took ~ " << elapsedms << " ms\n";
      output_trec(top_k, query.first, id_map, query_type, output); 
    }
}

typedef std::vector<std::pair<double, uint64_t>> top_k_list;

template<typename IndexType, typename WandType>
void rm_three_expansion_external(const char *index_filename,
              const char *ext_index_filename,
              const char *wand_data_filename,
              const char *ext_wand_data_filename,
              const char *forward_index_filename,
              const char *ext_forward_index_filename,
              std::vector<std::pair<uint32_t, ds2i::term_id_vec>> const &queries,
              std::vector<std::pair<uint32_t, ds2i::term_id_vec>> const &ext_queries,
              std::string const &type,
              std::string const &query_type,
              const char *map_filename,
              const char *ext_map_filename,
              const char *output_filename,
              const uint64_t m_k,
              const uint64_t exp_k,
              const uint64_t expand_term_count,
              const double r_weight,
              std::unordered_map<term_id_type, term_id_type>& back_map) {
    using namespace ds2i;

    /* Target Corpus Init */

    IndexType index;
    logger() << "Loading target index from " << index_filename << std::endl;
    boost::iostreams::mapped_file_source m(index_filename);
    succinct::mapper::map(index, m);

    document_index forward_index;
    logger() << "Loading target forward index from " << forward_index_filename << std::endl;
    forward_index.load(std::string(forward_index_filename));

    std::vector<std::string> doc_map;
    logger() << "Loading target map file from " << map_filename << std::endl;
    std::ifstream map_in(map_filename);
    std::string t_docid;
    while (map_in >> t_docid) {
      doc_map.emplace_back(t_docid); 
    }
    logger() << "Loaded " << doc_map.size() << " DocID's from target corpus" << std::endl; 
  
    logger() << "Warming up posting lists on target collection" << std::endl;
    std::unordered_set<term_id_type> warmed_up;
    for (auto const &q: queries) {
        for (auto t: q.second) {
            if (!warmed_up.count(t)) {
                index.warmup(t);
                warmed_up.insert(t);
            }
        }
    }

    /* External Corpus Init */

    IndexType ext_index;
    logger() << "Loading external index from " << ext_index_filename << std::endl;
    boost::iostreams::mapped_file_source ext_m(ext_index_filename);
    succinct::mapper::map(ext_index, ext_m);

    document_index ext_forward_index;
    logger() << "Loading external forward index from " << ext_forward_index_filename << std::endl;
    ext_forward_index.load(std::string(ext_forward_index_filename));

    std::vector<std::string> ext_doc_map;
    logger() << "Loading external map file from " << ext_map_filename << std::endl;
    std::ifstream ext_map_in(ext_map_filename);
    std::string ext_t_docid;
    while (ext_map_in >> ext_t_docid) {
      ext_doc_map.emplace_back(ext_t_docid); 
    }
    logger() << "Loaded " << ext_doc_map.size() << " DocID's from external corpus" << std::endl; 
  
    logger() << "Warming up posting lists on external collection" << std::endl;
    std::unordered_set<term_id_type> ext_warmed_up;
    for (auto const &ext_q: ext_queries) {
        for (auto ext_t: ext_q.second) {
            if (!ext_warmed_up.count(ext_t)) {
                ext_index.warmup(ext_t);
                ext_warmed_up.insert(ext_t);
            }
        }
    }

    /* Source Corpus Wand Init */

    WandType wdata;

    std::vector<std::string> query_types;
    boost::algorithm::split(query_types, query_type, boost::is_any_of(":"));
    boost::iostreams::mapped_file_source md;
    if (wand_data_filename) {
        md.open(wand_data_filename);
        succinct::mapper::map(wdata, md, succinct::mapper::map_flags::warmup);
    }

    /* External Corpus Wand Init */

    WandType ext_wdata;

    std::vector<std::string> ext_query_types;
    boost::algorithm::split(ext_query_types, query_type, boost::is_any_of(":"));
    boost::iostreams::mapped_file_source ext_md;
    if (ext_wand_data_filename) {
        ext_md.open(ext_wand_data_filename);
        succinct::mapper::map(ext_wdata, ext_md, succinct::mapper::map_flags::warmup);
    }

    /* Our output file */

    std::ofstream output_handle(output_filename);

    // Init our source ranker
    std::unique_ptr<doc_scorer> ranker = build_ranker(wdata.average_doclen(), 
                                                      wdata.num_docs(),
                                                      wdata.terms_in_collection(),
                                                      wdata.ranker_id());

    // Init our external ranker
    std::unique_ptr<doc_scorer> ext_ranker = build_ranker(ext_wdata.average_doclen(), 
                                                      ext_wdata.num_docs(),
                                                      ext_wdata.terms_in_collection(),
                                                      ext_wdata.ranker_id());

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
              auto tmp = wand_query<WandType>(ext_wdata, k_expand);
              tmp(ext_index, query, ext_ranker); 
              auto tk = tmp.topk();
              auto weighted_query = ext_forward_index.rm_expander(tk, expand_term_count);
              normalize_weighted_query_ext(weighted_query, back_map);
              query_from_ext_to_src(query, back_map);
              add_original_query(r_weight, weighted_query, query);
              auto final_traversal = weighted_maxscore_query<WandType>(wdata, k_final);
              final_traversal(index, weighted_query, ranker);
              return final_traversal.topk();
            };
        } else if (t == "block_max_wand" && wand_data_filename) {
            query_fun = [&](ds2i::term_id_vec query) {
              auto tmp = block_max_wand_query<WandType>(ext_wdata, k_expand);
              tmp(ext_index, query, ext_ranker); 
              auto tk = tmp.topk();
              auto weighted_query = ext_forward_index.rm_expander(tk, expand_term_count);
              normalize_weighted_query_ext(weighted_query, back_map);
              query_from_ext_to_src(query, back_map);
              add_original_query(r_weight, weighted_query, query);
              auto final_traversal = weighted_maxscore_query<WandType>(wdata, k_final);
              final_traversal(index, weighted_query, ranker);
              return final_traversal.topk();
            };
        }  else if (t == "ranked_or" && wand_data_filename) {
            query_fun = [&](ds2i::term_id_vec query) { 
              auto tmp = ranked_or_query<WandType>(ext_wdata, k_expand);
              tmp(ext_index, query, ext_ranker); 
              auto tk = tmp.topk();
              auto weighted_query = ext_forward_index.rm_expander(tk, expand_term_count);
              normalize_weighted_query_ext(weighted_query, back_map);
              query_from_ext_to_src(query, back_map);
              add_original_query(r_weight, weighted_query, query);
              auto final_traversal = weighted_maxscore_query<WandType>(wdata, k_final);
              final_traversal(index, weighted_query, ranker);
              return final_traversal.topk();
          };
        } else if (t == "maxscore" && wand_data_filename) {
            /* Our favorite 1 */
            query_fun = [&](ds2i::term_id_vec query) { 
              auto tmp = block_max_wand_query<WandType>(ext_wdata, k_expand); // USE BMW FOR STAGE 1
              tmp(ext_index, query, ext_ranker); 
              auto tk = tmp.topk();
              auto weighted_query = ext_forward_index.rm_expander(tk, expand_term_count);
              normalize_weighted_query_ext(weighted_query, back_map);
              query_from_ext_to_src(query, back_map);
              add_original_query(r_weight, weighted_query, query);
              auto final_traversal = weighted_maxscore_query<WandType>(wdata, k_final);
              final_traversal(index, weighted_query, ranker);
              return final_traversal.topk();
            };
        } else {
            logger() << "Unsupported query type: " << t << std::endl;
            break;
        }

        op_dump_trec(query_fun, ext_queries, doc_map, t, output_handle);
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
    const char *map_filename = nullptr;
    const char *lexicon_filename = nullptr;

    const char *ext_index_filename = argv[5];
    const char *ext_forward_filename = argv[6];
    const char *ext_wand_data_filename = nullptr;
    const char *ext_map_filename = nullptr;
    const char *ext_lexicon_filename = nullptr;

    const char *out_filename = nullptr;
    const char *query_filename = nullptr;

    uint64_t m_k = 0;
    uint64_t exp_k = 0;
    uint64_t exp_t = 0;
    double r_weight = 0;
    bool compressed = false;

    std::vector<std::pair<uint32_t, term_id_vec>> queries;
    std::vector<std::pair<uint32_t, term_id_vec>> ext_queries;

    for (int i = 5; i < argc; ++i) {
        std::string arg = argv[i];

        if(arg == "--wand"){
            wand_data_filename = argv[++i];;
        }

        if(arg == "--ext_wand"){
            ext_wand_data_filename = argv[++i];;
        }

        if (arg == "--map") {
            map_filename = argv[++i]; 
        }

        if (arg == "--ext_map") {
            ext_map_filename = argv[++i]; 
        }

        if (arg == "--lexicon") {
          lexicon_filename = argv[++i];
        }

        if (arg == "--ext_lexicon") {
          ext_lexicon_filename = argv[++i];
        }

        if (arg == "--output") {
          out_filename = argv[++i];
        }

        if(arg == "--compressed-wand"){
            compressed = true;
        }

        if (arg == "--query") {
            query_filename = argv[++i];
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
 
        
        if (arg == "--rweight") {
          r_weight = std::stod(argv[++i]);
        }
    }

    if (exp_k == 0 || m_k == 0 || exp_t == 0) {
        std::cerr << "Must set --texp --kexp and --kfinal" << std::endl;
        return EXIT_FAILURE;
    }

    if (out_filename == nullptr || map_filename == nullptr) {
      std::cerr << "ERROR: Must provide map and output file. Quitting."
                << std::endl;
      return EXIT_FAILURE;
    }

    if (lexicon_filename == nullptr || ext_lexicon_filename == nullptr || ext_map_filename == nullptr) {
      std::cerr << "ERROR: Must provide external map and source + target lexicon files. Quitting."
                << std::endl;
      return EXIT_FAILURE;
    }

    
    if (r_weight < 0 || r_weight > 1) {
      std::cerr << "Weight must be between 0 and 1/"
                << std::endl;
      return EXIT_FAILURE;
    }

    /* Target collection lexicon */
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

    /* Source collection lexicon */
    std::unordered_map<std::string, uint32_t> ext_lexicon;
    if (ext_lexicon_filename != nullptr) {
      std::ifstream ext_in_lex(ext_lexicon_filename);
      if (ext_in_lex.is_open()) {
        read_lexicon(ext_in_lex, ext_lexicon);
      }
      else {
        std::cerr << "ERROR: Could not open lexicon file." << std::endl;
      }
    }

    /* Build the back map */
    std::unordered_map<term_id_type, term_id_type> back_map;

    for (auto it : ext_lexicon) { 
        std::unordered_map<std::string, uint32_t>::const_iterator got = lexicon.find (it.first);
        if ( got != lexicon.end() ) {
            back_map.emplace(it.second, got->second);
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

    term_id_vec ext_q;
    uint32_t ext_qid;
    if(query_filename){
        std::filebuf ext_fb;
        if (ext_fb.open(query_filename, std::ios::in)) {
            std::istream ext_is(&ext_fb);
            while (read_query(ext_q, ext_qid, ext_lexicon, ext_is)) ext_queries.emplace_back(ext_qid, ext_q);
        }
    } 

    /**/
    if (false) {
#define LOOP_BODY(R, DATA, T)                                                       \
        } else if (type == BOOST_PP_STRINGIZE(T)) {                                 \
            if (compressed) {                                                       \
                 rm_three_expansion_external<BOOST_PP_CAT(T, _index), wand_uniform_index>              \
                 (index_filename, ext_index_filename, wand_data_filename, ext_wand_data_filename, forward_filename, ext_forward_filename, queries, ext_queries, type, query_type, map_filename, ext_map_filename, out_filename, m_k, exp_k, exp_t, r_weight, back_map);   \
            } else {                                                                \
                rm_three_expansion_external<BOOST_PP_CAT(T, _index), wand_raw_index>                   \
                (index_filename, ext_index_filename, wand_data_filename, ext_wand_data_filename, forward_filename, ext_forward_filename, queries, ext_queries, type, query_type, map_filename, ext_map_filename, out_filename, m_k, exp_k, exp_t, r_weight, back_map);    \
            }                                                                       \
    /**/

BOOST_PP_SEQ_FOR_EACH(LOOP_BODY, _, DS2I_INDEX_TYPES);
#undef LOOP_BODY

    } else {
        logger() << "ERROR: Unknown type " << type << std::endl;
    }

}
