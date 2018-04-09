
#pragma once

#include "succinct/mappable_vector.hpp"

#include "binary_freq_collection.hpp"
#include "rankers.hpp"
#include "util.hpp"
#include "wand_utils.hpp"

namespace ds2i {

class wand_data_raw {
public:
    wand_data_raw() {}

    class builder {
    public:
        builder(partition_type type, binary_freq_collection const& coll, global_parameters const& params)
        {
            (void)coll;
            (void)params;
            this->type = type;
            logger() << "Storing max weight for each list and for each block..." << std::endl;
            total_elements = 0;
            total_blocks = 0;
            effective_list = 0;
            blocks_start.push_back(0);
        }

        std::pair<float, float> 
        add_sequence(binary_freq_collection::sequence const& seq, std::vector<float> const& norm_lens,
                    const uint32_t term_ctf, std::unique_ptr<doc_scorer>& ranker)
        {

            // JMM: Always use wand, do not allow non-wand lists
            auto t = ((type == partition_type::fixed_blocks) ? static_block_partition(seq, norm_lens, term_ctf, ranker)
                                                             : variable_block_partition(seq, norm_lens, term_ctf, ranker));

            block_max_term_weight.insert(block_max_term_weight.end(), std::get<2>(t).begin(),
                std::get<2>(t).end());
            block_docid.insert(block_docid.end(), std::get<1>(t).begin(), std::get<1>(t).end());
            max_term_weight = (*(std::max_element(std::get<2>(t).begin(), std::get<2>(t).end())));
            blocks_start.push_back(std::get<1>(t).size() + blocks_start.back());
            block_max_document_weight.insert(block_max_document_weight.end(), std::get<3>(t).begin(),
                std::get<3>(t).end());
            max_document_weight = (*(std::max_element(std::get<3>(t).begin(), std::get<3>(t).end())));
            
            total_elements += seq.docs.size();
            total_blocks += std::get<1>(t).size();
            effective_list++;

            return std::make_pair(max_term_weight, max_document_weight);
        }

        void build(wand_data_raw& wdata)
        {
            wdata.m_block_max_term_weight.steal(block_max_term_weight);
            wdata.m_block_max_document_weight.steal(block_max_document_weight);
            wdata.m_blocks_start.steal(blocks_start);
            wdata.m_block_docid.steal(block_docid);
            logger() << "number of elements / number of blocks: " << (float)total_elements / (float)total_blocks << std::endl;
        }

        partition_type type;
        uint64_t total_elements;
        uint64_t total_blocks;
        uint64_t effective_list;
        float max_term_weight;
        float max_document_weight;
        std::vector<uint64_t> blocks_start;
        std::vector<uint32_t> block_docid;
        std::vector<float> block_max_term_weight;
        std::vector<float> block_max_document_weight;
    };
    class enumerator {
        friend class wand_data_raw;

    public:
        enumerator(uint32_t _block_start, uint32_t _block_number, succinct::mapper::mappable_vector<float> const& max_term_weight,
            succinct::mapper::mappable_vector<float> const& max_document_weight,
            succinct::mapper::mappable_vector<uint32_t> const& block_docid)
            : cur_pos(0)
            , block_start(_block_start)
            , block_number(_block_number)
            , m_block_max_term_weight(max_term_weight)
            , m_block_max_document_weight(max_document_weight)
            , m_block_docid(block_docid)
        {
        }

        void DS2I_NOINLINE next_geq(uint64_t lower_bound)
        {
            while (cur_pos + 1 < block_number && m_block_docid[block_start + cur_pos] < lower_bound) {
                cur_pos++;
            }
        }

        void DS2I_FLATTEN_FUNC next()
        {
            if (cur_pos + 1 < block_number) {
                cur_pos++;
            }
        }

        uint64_t DS2I_FLATTEN_FUNC size()
        {
            return block_number;
        }

        float DS2I_FLATTEN_FUNC score() const
        {
            return m_block_max_term_weight[block_start + cur_pos];
        }

        float DS2I_FLATTEN_FUNC doc_weight() const
        {
            return m_block_max_document_weight[block_start + cur_pos];
        }

        uint64_t DS2I_FLATTEN_FUNC docid() const
        {
            return m_block_docid[block_start + cur_pos];
        }

        uint64_t DS2I_FLATTEN_FUNC find_next_skip()
        {
            return m_block_docid[cur_pos + block_start];
        }

    private:
        uint64_t cur_pos;
        uint64_t block_start;
        uint64_t block_number;
        succinct::mapper::mappable_vector<float> const& m_block_max_term_weight;
        succinct::mapper::mappable_vector<float> const& m_block_max_document_weight;
        succinct::mapper::mappable_vector<uint32_t> const& m_block_docid;
    };

    enumerator get_enum(uint32_t i) const
    {
        return enumerator(m_blocks_start[i], m_blocks_start[i + 1] - m_blocks_start[i], m_block_max_term_weight, m_block_max_document_weight, m_block_docid);
    }

    template <typename Visitor>
    void map(Visitor& visit)
    {
        visit(m_blocks_start, "m_blocks_start")
             (m_block_max_term_weight, "m_block_max_term_weight")
             (m_block_max_document_weight, "m_block_max_document_weight")
             (m_block_docid, "m_block_docid");
    }

private:
    succinct::mapper::mappable_vector<uint64_t> m_blocks_start;
    succinct::mapper::mappable_vector<float> m_block_max_term_weight;
    succinct::mapper::mappable_vector<float> m_block_max_document_weight;
    succinct::mapper::mappable_vector<uint32_t> m_block_docid;
};
}
