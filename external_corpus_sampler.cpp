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
#include "document_fuser.hpp" // RRF fusion
#include "collection_config.hpp"
#include "weighted_sampler.hpp"

namespace {
void printUsage(const std::string &programName) {
  std::cerr << "Usage: " << programName
            << " index_type query_algorithm[ignored] target_collection_param --external external_collection_param [can have n of these]"
            << " --query query_filename --output output_file" << std::endl;
}
} // namespace

using namespace ds2i;

typedef std::vector<std::pair<double, uint64_t>> top_k_list;

void do_join(std::thread& t)
{
    t.join();
}

template<typename IndexType, typename WandType>
struct collection_data {
    
    // Collection data
    boost::iostreams::mapped_file_source m;
    boost::iostreams::mapped_file_source mw;
    std::unique_ptr<IndexType> invidx;
    std::unique_ptr<WandType> wdata; 
    std::unique_ptr<document_index> forward_index;
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

    // Generation/Sampling params
    weighted_sampler *sampler;
    uint64_t gen_queries; 

    // Target?
    bool target;


    collection_data () {}

    collection_data (const collection_config& conf, weighted_sampler *samp) 
                    : docs_to_expand(conf.m_docs_to_expand), 
                      terms_to_expand(conf.m_terms_to_expand),
                      final_k(conf.m_final_k),
                      sampler(samp),
                      gen_queries(conf.m_gen_queries),
                      target(conf.m_target)
    {
        // 1. Open inverted index and load
        logger() << "Loading index from " << conf.m_invidx_file << std::endl;
        invidx = std::unique_ptr<IndexType>(new IndexType);
        m = boost::iostreams::mapped_file_source(conf.m_invidx_file.c_str());
        succinct::mapper::map(*invidx, m);
    
        // 2. Load forward index
        logger() << "Loading forward index from " << conf.m_fidx_file << std::endl;
        forward_index = std::unique_ptr<document_index>(new document_index);
        //document_index forward_index;
        (*forward_index).load(conf.m_fidx_file);

        // 3. Wand data
        logger() << "Loading wand data from " << conf.m_wand_file << std::endl;
        wdata = std::unique_ptr<WandType>(new WandType);
        mw = boost::iostreams::mapped_file_source(conf.m_wand_file.c_str());
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

        // Only required for the target collection, builds TREC docname map
        if (target) {
            logger() << "Loading map file from " << conf.m_map_file << std::endl;
            std::ifstream map_in(conf.m_map_file);
            std::string t_docid;
            while (map_in >> t_docid) {
                doc_map.emplace_back(t_docid); 
            }
        }
    }

    // Builds a way to map external collection term ids to target collection term ids
    void build_term_map(const std::unordered_map<std::string, uint32_t>& target_lexicon) {
 
        for (auto it : target_lexicon) { 
            auto got = lexicon.find(it.first);
            if ( got != lexicon.end() ) {
                back_map.emplace(got->second, it.second);
            }
        }
    }
    
    // Run RM on the external corpus, find candidate terms, and map back into the
    // target collection
    // Currently hardcoded to use BMW traversal for the bag-of-words
    std::vector<term_id_vec> run_rm_sampler() {
        auto tmp = block_max_wand_query<WandType>(*wdata, docs_to_expand);
        tmp(*invidx, parsed_query, ranker); 
        auto tk = tmp.topk();
        auto weighted_query = (*forward_index).rm_expander(tk, terms_to_expand);
        std::vector<term_id_vec> new_bow;
        if (!target) {
            // Convert to target vocabulary
            normalize_weighted_query_ext(weighted_query, back_map);
            query_from_ext_to_src(parsed_query, back_map);
            // Generate query batch
            new_bow = sampler->generate_query_batch(weighted_query, parsed_query, 5, 15, gen_queries); 
        }
        else {
            normalize_weighted_query(weighted_query);
            new_bow = sampler->generate_query_batch(weighted_query, parsed_query, 5, 15, gen_queries); 
        }

        return new_bow;
    } 
   
    // Final run, currently hardcoded to use MaxScore (Unweighted)
    top_k_list final_run (term_id_vec& bow_query) {
        auto final_traversal = maxscore_query<WandType>(*wdata, final_k);
        final_traversal(*invidx, bow_query, ranker);
        return final_traversal.topk();
    }

};

// Assume all indexes/wand files use the same type
template<typename IndexType, typename WandType>
void external_sample(std::vector<collection_config>& collection_conf,
              std::string query_file,
              std::string const &type,
              std::string const &query_type,
              std::string output_filename,
              uint64_t seed) {
    using cdata = collection_data<IndexType, WandType>;
   
    // Create a single sampler object with seed
    weighted_sampler query_sampler(seed);
    

    // Get the collections ready
    std::vector<collection_data<IndexType, WandType>> all_collections;
    collection_data<IndexType, WandType> *target_handle;
    
    
    // Build each collection
    for (size_t i = 0; i < collection_conf.size(); ++i) {
        all_collections.emplace_back(collection_conf[i], &query_sampler);
    } 


    // Get handle on target and ensure it is indeed the target. We can assume
    // target_handle is a valid pointer as long as we don't change the size
    // of the all_collections container
    target_handle = &(all_collections[0]);
    if (!target_handle->target) {
        std::cerr << "Target handle is not on target collection. Exiting."
                  << std::endl;
    }

    // Iterate all non-targets and build term mapper
    for (size_t i = 1; i < all_collections.size(); ++i) {
        all_collections[i].build_term_map(target_handle->lexicon);
    } 

    // Prepare output stream
    std::ofstream output_handle(output_filename);

    // Read query file
    std::ifstream qs(query_file);
    std::map<uint32_t, std::vector<std::string>> queries;
    read_string_query_file(queries, qs);
    std::cerr << "Read " << queries.size() << " queries.\n";
   
    std::map<uint32_t, double> query_times;

    size_t runs = 1; // TIMINGS
    for (size_t r = 0; r < runs; ++r) {
  
        for (const auto &query : queries) {
       
            // 0. Begin time block here XXX 
            auto tick = get_time_usecs();

            // 1. Parse and set query for each collection
            for (auto &coll : all_collections) {
                coll.parsed_query = parse_query(query.second, coll.lexicon);
            }

            // 2. Run the RM process and generate queries
            std::vector<std::thread> my_threads;
            std::vector<std::vector<term_id_vec>> all_q(all_collections.size());
            // Exclude bucket 0 because this is the target collection
            for (size_t bucket = 1; bucket < all_collections.size(); ++bucket) {
                auto q_thread = std::thread([&, bucket]() {
                    all_q[bucket] = all_collections[bucket].run_rm_sampler();
                });
                my_threads.emplace_back(std::move(q_thread));
            }

            // Join the workers
            std::for_each(my_threads.begin(), my_threads.end(), do_join);
            my_threads.clear();

            std::vector<term_id_vec*> all_subqueries;
            for (size_t i = 0; i < all_q.size(); i++) {
                for(size_t j = 0; j < all_q[i].size(); ++j) {
                    all_subqueries.emplace_back(&all_q[i][j]);
                }
            }
            // Now just add into the mix the title query that the user entered
            all_subqueries.emplace_back(&target_handle->parsed_query);

            // Run all sub-queries on the target 
            std::vector<top_k_list> final_trec_runs(all_subqueries.size());
            for (size_t bucket = 0; bucket < all_subqueries.size(); ++bucket) {
                auto q_thread = std::thread([&, bucket]() {
                    final_trec_runs[bucket] = target_handle->final_run(*all_subqueries[bucket]);
                });
                my_threads.emplace_back(std::move(q_thread));
            }
 
            // Join the workers
            std::for_each(my_threads.begin(), my_threads.end(), do_join);
            my_threads.clear(); 

            // 3. Now we can fuse
            top_k_list final_ranking;
            document_fuser::hot_fuse(final_trec_runs, final_ranking);
            if (final_ranking.size() > target_handle->final_k) {
                final_ranking.resize(target_handle->final_k);
            } 
        
            // 4. End timing block XXX
            auto tock = get_time_usecs();
            double elapsed = (tock-tick);
            //std::cout<< query.first << "," << elapsedms << std::endl;

            if (r == 0) {
              output_trec(final_ranking, query.first, target_handle->doc_map, "ExternalRMSampler", output_handle); 
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


    return;
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
    std::string query_type = argv[2];
    std::string target_param = argv[3];
    std::string query_file = "";
    std::string output_file = "";
    std::vector<std::string> external_param;
    bool compressed = false;
    size_t seed = 1000;

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

        if (arg == "--seed") {
            seed = std::stoull(argv[++i]);
            std::cerr << "Random seed = " << seed << std::endl; 
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
                 external_sample<BOOST_PP_CAT(T, _index), wand_uniform_index>              \
                 (conf, query_file, type, query_type, output_file, seed);   \
            } else {                                                                \
                external_sample<BOOST_PP_CAT(T, _index), wand_raw_index>                   \
                 (conf, query_file, type, query_type, output_file, seed);   \
            }                                                                       \
    /**/

BOOST_PP_SEQ_FOR_EACH(LOOP_BODY, _, DS2I_INDEX_TYPES);
#undef LOOP_BODY

    } else {
        logger() << "ERROR: Unknown type " << type << std::endl;
    }

}
