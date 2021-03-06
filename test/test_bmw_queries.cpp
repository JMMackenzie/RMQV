#define BOOST_TEST_MODULE bwm_queries

#include "succinct/test_common.hpp"
#include <boost/test/floating_point_comparison.hpp>

#include "ds2i_config.hpp"
#include "index_types.hpp"
#include "queries.hpp"

namespace ds2i {
    namespace test {

        struct index_initialization {

            typedef opt_index index_type;
            typedef wand_data<bm25, wand_data_compressed<bm25, uniform_score_compressor>> WandTypeUniform;
            typedef wand_data<bm25, wand_data_raw<bm25>> WandTypePlain;


            index_initialization()
                    : collection(DS2I_SOURCE_DIR "/test/test_data/test_collection"),
                      document_sizes(DS2I_SOURCE_DIR "/test/test_data/test_collection.sizes"),
                      wdata(document_sizes.begin()->begin(), collection.num_docs(), collection, partition_type::variable_blocks),
                      wdata_fixed(document_sizes.begin()->begin(), collection.num_docs(), collection, partition_type::fixed_blocks),
                      wdata_uniform(document_sizes.begin()->begin(), collection.num_docs(), collection, partition_type::variable_blocks) {
                index_type::builder builder(collection.num_docs(), params);
                for (auto const &plist: collection) {
                    uint64_t freqs_sum = std::accumulate(plist.freqs.begin(),
                                                         plist.freqs.end(), uint64_t(0));
                    builder.add_posting_list(plist.docs.size(), plist.docs.begin(),
                                             plist.freqs.begin(), freqs_sum);
                }
                builder.build(index);

                term_id_vec q;
                std::ifstream qfile(DS2I_SOURCE_DIR "/test/test_data/queries");
                while (read_query(q, qfile)) queries.push_back(q);
            }

            global_parameters params;
            binary_freq_collection collection;
            binary_collection document_sizes;
            index_type index;
            std::vector<term_id_vec> queries;
            WandTypePlain wdata;
            WandTypePlain wdata_fixed;
            WandTypeUniform wdata_uniform;


            template<typename QueryOp>
            void test_against_wand(QueryOp &op_q) const {
                wand_query<WandTypePlain> or_q(wdata, 10);

                for (auto const &q: queries) {
                    or_q(index, q);
                    op_q(index, q);
                    BOOST_REQUIRE_EQUAL(or_q.topk().size(), op_q.topk().size());

                    for (size_t i = 0; i < or_q.topk().size(); ++i) {
                        BOOST_REQUIRE_CLOSE(or_q.topk()[i].first, op_q.topk()[i].first, 0.01); // tolerance is % relative
                    }
                    op_q.clear_topk();
                }
            }


        };

    }
}


BOOST_FIXTURE_TEST_CASE(block_max_wand,
                        ds2i::test::index_initialization) {
    ds2i::block_max_wand_query<WandTypePlain> block_max_wand_q(wdata, 10);
    ds2i::block_max_wand_query<WandTypeUniform> block_max_wand_uniform_q(wdata_uniform, 10);
    ds2i::block_max_wand_query<WandTypePlain> block_max_wand_fixed_q(wdata_fixed, 10);
    test_against_wand(block_max_wand_uniform_q);
    test_against_wand(block_max_wand_q);
    test_against_wand(block_max_wand_fixed_q);
}

