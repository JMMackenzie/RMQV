#include <iostream>
#include <unordered_map>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>

#include <succinct/mapper.hpp>

#include "benchmark.h"
#include "index_types.hpp"
#include "queries.hpp"
#include "queries_util.hpp"
#include "util.hpp"
#include "wand_data_compressed.hpp"
#include "wand_data_raw.hpp"

namespace {
void printUsage(const std::string& programName)
{
    std::cerr << "Usage: " << programName
              << " --wand wand_data_filename"
              << " --query query_filename"
              << " [--compressed-wand] [--lexicon lexicon_file]" << std::endl;
}
} // namespace

using namespace ds2i;

template <typename WandType>
void analyze_queries(const char* wand_data_filename, std::vector<std::pair<uint32_t, ds2i::term_id_vec> > const& queries)
{
    using namespace ds2i;

    WandType wdata;

    // Read the wand data
    std::cout << "Load wand data from " << wand_data_filename << std::endl;
    boost::iostreams::mapped_file_source md;
    if (wand_data_filename) {
        md.open(wand_data_filename);
        succinct::mapper::map(wdata, md, succinct::mapper::map_flags::warmup);
    }

    // Init our ranker
    std::cout << "Create ranker" << std::endl;
    std::unique_ptr<doc_scorer> ranker = build_ranker(wdata.average_doclen(),
        wdata.num_docs(),
        wdata.terms_in_collection(),
        wdata.ranker_id());

    std::cout << "Process queries" << std::endl;
    for (auto const& query : queries) {
        auto qry_copy = query.second;
        std::sort(qry_copy.begin(), qry_copy.end());
        auto last = std::unique(qry_copy.begin(), qry_copy.end());
        qry_copy.erase(last, qry_copy.end());
        // count query term frequencies
        std::vector<float> query_block_weights;
        for (auto qterm_id : qry_copy) {
            auto w_enum = wdata.getenum(qterm_id);
            std::vector<float> qid_weights(w_enum.size());
            for (size_t i = 0;i< w_enum.size(); i++) {
                qid_weights[i] = w_enum.score();
		w_enum.next();
            }
            auto max_score = *std::max_element(qid_weights.begin(), qid_weights.end());
            std::for_each(qid_weights.begin(), qid_weights.end(), [&max_score](float& score) { score = score / max_score; });
            query_block_weights.insert(query_block_weights.end(), qid_weights.begin(), qid_weights.end());
        }
        std::sort(query_block_weights.begin(), query_block_weights.end());
        std::for_each(query_block_weights.begin(), query_block_weights.end(), [](float& score) { score = std::round(score * 100 / 5); });
        std::vector<std::pair<uint32_t, float> > qry_score_dist;
        auto prev = query_block_weights[0];
        size_t cur_cnt = 1;
        for (size_t i = 1; i < query_block_weights.size(); i++) {
            if (query_block_weights[i] != prev) {
                qry_score_dist.push_back({ prev, float(cur_cnt) / float(query_block_weights.size()) });
                cur_cnt = 1;
            } else {
                cur_cnt++;
            }
            prev = query_block_weights[i];
        }
        qry_score_dist.push_back({ prev, float(cur_cnt) / float(query_block_weights.size()) });

        for (const auto& qdst : qry_score_dist) {
            std::cout << ranker->name() << "," << query.first << "," << qdst.first << "," << qdst.second << std::endl;
        }
    }
}

typedef wand_data<wand_data_raw> wand_raw_index;
typedef wand_data<wand_data_compressed<uniform_score_compressor> > wand_uniform_index;

int main(int argc, const char** argv)
{
    using namespace ds2i;
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }

    const char* wand_data_filename = nullptr;
    const char* query_filename = nullptr;
    const char* lexicon_filename = nullptr;
    bool compressed = false;
    std::vector<std::pair<uint32_t, term_id_vec> > queries;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--wand") {
            wand_data_filename = argv[++i];
        }

        if (arg == "--compressed-wand") {
            compressed = true;
        }

        if (arg == "--query") {
            query_filename = argv[++i];
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
        } else {
            std::cerr << "ERROR: Could not open lexicon file." << std::endl;
        }
    }

    term_id_vec q;
    uint32_t qid;
    if (query_filename) {
        std::filebuf fb;
        if (fb.open(query_filename, std::ios::in)) {
            std::istream is(&fb);
            if (lexicon_filename != nullptr) {
                while (read_query(q, qid, lexicon, is))
                    queries.emplace_back(qid, q);
            } else {
                while (read_query(q, qid, is))
                    queries.emplace_back(qid, q);
            }
        }
    } else {
        if (lexicon_filename != nullptr) {
            while (read_query(q, qid, lexicon))
                queries.emplace_back(qid, q);
        } else {
            while (read_query(q, qid))
                queries.emplace_back(qid, q);
        }
    }
    std::cout << "read " << queries.size() << " queries." << std::endl;

    if (compressed) {
        analyze_queries<wand_uniform_index>(wand_data_filename, queries);
    } else {
        analyze_queries<wand_raw_index>(wand_data_filename, queries);
    }
}
