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
            << " index_type query_algorithm target_collection_param --external external_collection_param [can have n of these]"
            << " --query query_filename --output output_file" << std::endl;
}
} // namespace

using namespace ds2i;

void output_trec(const std::vector<std::pair<double, uint64_t>>& top_k,
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
      auto tick = get_time_usecs();
      top_k = query_func(query.second); // All stages
      auto tock = get_time_usecs();
      double elapsedms = (tock-tick)/1000;
      std::cerr << query.first << " took ~ " << elapsedms << " ms\n";
      output_trec(top_k, query.first, id_map, query_type, output); 
    }
}

/*
template<typename Functor>
void op_extern_rm(Functor query_func,
                  std::string query_file,

    

    for (auto const
*/

typedef std::vector<std::pair<double, uint64_t>> top_k_list;

void do_join(std::thread& t)
{
    t.join();
}

using namespace ds2i;

template<typename IndexType, typename WandType>
struct collection_data {
    
    // Collection data
    std::unique_ptr<IndexType> invidx;
    std::unique_ptr<WandType> wdata; 
    //IndexType invidx;
    //WandType wdata;
    std::unique_ptr<document_index> forward_index;
    //document_index forward_index;
    std::unique_ptr<doc_scorer> ranker;
    std::unordered_map<std::string, uint32_t> lexicon;
    std::vector<std::string> doc_map;
    std::unordered_map<uint32_t, uint32_t> back_map;

    // Query data
    std::vector<uint32_t> parsed_query;

    // Expansion params
    uint64_t docs_to_expand;
    uint64_t terms_to_expand; 
    uint64_t final_k; // only used in target
    double lambda; // weight for original term

    // Target?
    bool target;

   
/* 
    collection_data (const collection_data&& coll_dat) {
        std::cerr << "MOVE Construction\n" ;
        invidx = std::move(coll_dat.invidx);
        wdata = std::move(coll_dat.wdata);
        forward_index = std::move(coll_dat.forward_index);
        ranker = std::move(coll_dat.ranker);
        lexicon = std::move(coll_dat.lexicon);
        doc_map = std::move(coll_dat.doc_map);
        back_map = std::move(coll_dat.back_map);
        parsed_query = std::move(coll_dat.parsed_query);
        docs_to_expand = coll_dat.docs_to_expand;
        terms_to_expand = coll_dat.terms_to_expand;
        final_k = coll_dat.final_k;
        lambda = coll_dat.lambda;
        target = coll_dat.target;
    }    
*/
    collection_data () {}

    collection_data (const collection_config& conf, 
                     const std::unordered_map<std::string, uint32_t>& target_lexicon 
                     = std::unordered_map<std::string, uint32_t>()) 
                    : docs_to_expand(conf.m_docs_to_expand), 
                      terms_to_expand(conf.m_terms_to_expand),
                      final_k(conf.m_final_k),
                      lambda(conf.m_lambda),
                      target(conf.m_target)
    {
        // 1. Open inverted index and load
        logger() << "Loading index from " << conf.m_invidx_file << std::endl;
        invidx = std::unique_ptr<IndexType>(new IndexType);
        boost::iostreams::mapped_file_source m(conf.m_invidx_file.c_str());
        succinct::mapper::map(*invidx, m);
    
        // 2. Load forward index
        logger() << "Loading forward index from " << conf.m_fidx_file << std::endl;
        forward_index = std::unique_ptr<document_index>(new document_index);
        //document_index forward_index;
        (*forward_index).load(conf.m_fidx_file);

        // 3. Wand data
        logger() << "Loading wand data from " << conf.m_wand_file << std::endl;
        wdata = std::unique_ptr<WandType>(new WandType);
        boost::iostreams::mapped_file_source mw(conf.m_wand_file.c_str());
        succinct::mapper::map(*wdata, mw, succinct::mapper::map_flags::warmup);

        // 4. Ranker      
        //std::unique_ptr<doc_scorer> 
        ranker = build_ranker(wdata->average_doclen(), 
                                                          wdata->num_docs(),
                                                          wdata->terms_in_collection(),
                                                          wdata->ranker_id());
        // 5. Lexicon
        std::ifstream in_lex(conf.m_lexicon_file);
        read_lexicon(in_lex, lexicon);

        // Only required for the target collection
        if (target) {
            logger() << "Loading map file from " << conf.m_map_file << std::endl;
            std::ifstream map_in(conf.m_map_file);
            std::string t_docid;
            while (map_in >> t_docid) {
                doc_map.emplace_back(t_docid); 
            }
        }

        // Only required for non-targets
        else {
            for (auto it : target_lexicon) { 
                auto got = lexicon.find(it.first);
                if ( got != lexicon.end() ) {
                    back_map.emplace(it.second, got->second);
                }
            }
        }
    }

    weight_query run_rm() {
        auto tmp = wand_query<WandType>(*wdata, docs_to_expand);
        tmp(*invidx, parsed_query, ranker); 
        auto tk = tmp.topk();
        auto weighted_query = (*forward_index).rm_expander(tk, terms_to_expand);
        if (!target) {
            normalize_weighted_query_ext(weighted_query, back_map);
            query_from_ext_to_src(parsed_query, back_map);
            add_original_query(lambda, weighted_query, parsed_query);
        }
        else {
            normalize_weighted_query(weighted_query);
            add_original_query(lambda, weighted_query, parsed_query);
        }
        return weighted_query;
    } 
    
    top_k_list final_run (weight_query& w_query) {
        auto final_traversal = weighted_maxscore_query<WandType>(*wdata, final_k);
        final_traversal(*invidx, w_query, ranker);
        return final_traversal.topk();
    }

};

// Assume all indexes/wand files use the same type
template<typename IndexType, typename WandType>
void external_expansion(std::vector<collection_config>& collection_conf,
              std::string query_file,
              std::string const &type,
              std::string const &query_type,
              std::string output_filename) {
    using cdata = collection_data<IndexType, WandType>;
    // Get the collections ready
    std::vector<collection_data<IndexType, WandType>> all_collections;
    collection_data<IndexType, WandType> *target_handle;


  #define POC
  #ifdef POC
  auto conf = collection_conf[0];

        // 1. Open inverted index and load
        logger() << "Loading index from " << conf.m_invidx_file << std::endl;
        auto invidx = std::unique_ptr<IndexType>(new IndexType);
        boost::iostreams::mapped_file_source m(conf.m_invidx_file.c_str());
        succinct::mapper::map(*invidx, m);
    
        // 2. Load forward index
        logger() << "Loading forward index from " << conf.m_fidx_file << std::endl;
        auto forward_index = std::unique_ptr<document_index>(new document_index);
        //document_index forward_index;
        (*forward_index).load(conf.m_fidx_file);

        // 3. Wand data
        logger() << "Loading wand data from " << conf.m_wand_file << std::endl;
        auto wdata = std::unique_ptr<WandType>(new WandType);
        boost::iostreams::mapped_file_source mw(conf.m_wand_file.c_str());
        succinct::mapper::map(*wdata, mw, succinct::mapper::map_flags::warmup);

        // 4. Ranker      
        //std::unique_ptr<doc_scorer> 
        auto ranker = build_ranker(wdata->average_doclen(), 
                                                          wdata->num_docs(),
                                                          wdata->terms_in_collection(),
                                                          wdata->ranker_id());
        // 5. Lexicon
        std::ifstream in_lex(conf.m_lexicon_file);
        std::unordered_map<std::string, uint32_t> lexicon;
        read_lexicon(in_lex, lexicon);

    // Read query file
    std::ifstream qs(query_file);
    std::map<uint32_t, std::vector<std::string>> queries;
    read_string_query_file(queries, qs);
    std::cerr << "Read " << queries.size() << " queries.\n";


        for (const auto &query : queries) {
          auto q = parse_query(query.second, lexicon);
       
          auto tmp = wand_query<WandType>(*wdata, 100);
          tmp(*invidx, q, ranker); 
          auto tk = tmp.topk();
        }

}



/*
    // Processing type
    std::function<std::vector<std::pair<double, uint64_t>>(ds2i::term_id_vec)> query_fun;
 
    if (t == "wand") {

        query_fun = [&](ds2i::term_id_vec query) {
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

    } else if (t == "block_max_wand") {
            query_fun = [&](ds2i::term_id_vec query) {
              auto tmp = block_max_wand_query<WandType>(wdata, k_expand);
              tmp(index, query, ranker);
              auto tk = tmp.topk();
              auto weighted_query = forward_index.rm_expander(tk, expand_term_count);
              normalize_weighted_query(weighted_query);
              add_original_query(r_weight, weighted_query, query);
              auto final_traversal = weighted_maxscore_query<WandType>(wdata, k_final);
              final_traversal(index, weighted_query, ranker);
              return final_traversal.topk();
            };
        }  else if (t == "ranked_or") {
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
        } else if (t == "maxscore") {
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
}*/

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
    std::string query_type = argv[2];
    std::string target_param = argv[3];
    std::string query_file = "";
    std::string output_file = "";
    std::vector<std::string> external_param;
    bool compressed = false;

    for (int i = 4; i < argc; ++i) {
        std::string arg = argv[i];

        if(arg == "--compressed-wand"){
            compressed = true;
        }

        if (arg == "--query") {
            query_file = argv[++i];
        }

        if (arg == "--output") {
            output_file = argv[++i];
        }

        if (arg == "--external") {
            std::string x = argv[++i];
            external_param.push_back(x);
        }
    }

    if (output_file == "" or query_file == "") {
      std::cerr << "ERROR: Must provide both query and output file. Quitting." << std::endl;
      return EXIT_FAILURE;
    }
   
    // Read config data
    std::vector<collection_config> conf;
    std::ifstream in_target(target_param);
    conf.emplace_back(in_target, true);
    in_target.close();
    for (size_t i = 0; i < external_param.size(); i++) {
        std::ifstream in_ex(external_param[i]);
        conf.emplace_back(in_ex, false);
    }

    //external_expansion<opt_index, wand_raw_index>
    //  (conf, query_file, type, query_type, output_file);
    //return;

    // Call and run the real "fun"
    /**/
    if (false) {
#define LOOP_BODY(R, DATA, T)                                                       \
        } else if (type == BOOST_PP_STRINGIZE(T)) {                                 \
            if (compressed) {                                                       \
                 external_expansion<BOOST_PP_CAT(T, _index), wand_uniform_index>              \
                 (conf, query_file, type, query_type, output_file);   \
            } else {                                                                \
                external_expansion<BOOST_PP_CAT(T, _index), wand_raw_index>                   \
                 (conf, query_file, type, query_type, output_file);   \
            }                                                                       \
    /**/

BOOST_PP_SEQ_FOR_EACH(LOOP_BODY, _, DS2I_INDEX_TYPES);
#undef LOOP_BODY

    } else {
        logger() << "ERROR: Unknown type " << type << std::endl;
    }

}
