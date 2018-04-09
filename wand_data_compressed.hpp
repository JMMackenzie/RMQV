#pragma once

#include "succinct/mappable_vector.hpp"

#include "binary_freq_collection.hpp"
#include "bitvector_collection.hpp"
#include "configuration.hpp"
#include "rankers.hpp"
#include "score_partitioning.hpp"
#include "util.hpp"

#include "global_parameters.hpp"
#include "index_build_utils.hpp"
#include "positive_sequence.hpp"
#include "wand_utils.hpp"

namespace ds2i {
namespace {
    static const size_t score_bits_size = succinct::broadword::msb(configuration::get().reference_size);
}

class uniform_score_compressor {

public:
    class builder {
    public:
        builder(uint64_t num_docs, global_parameters const& params)
            : m_params(params)
            , m_num_docs((num_docs + 1) << score_bits_size)
            , m_docs_sequences(params)
        {
        }

        std::vector<uint32_t> compress_data(std::vector<float> effective_scores)
        {
            float quant = 1.f / configuration::get().reference_size;

            //Partition scores.
            std::vector<uint32_t> score_indexes;
            score_indexes.reserve(effective_scores.size());
            for (const auto& score : effective_scores) {
                size_t pos = 1;
                while (score > quant * pos)
                    pos++;
                score_indexes.push_back(pos - 1);
            }
            return score_indexes;
        }

        template <typename Sequence = compact_elias_fano, typename DocsIterator>
        void add_posting_list(uint64_t n, DocsIterator docs_begin, DocsIterator score_begin)
        {
            std::vector<uint64_t> temp;
            for (size_t i = 0; i < n; ++i) {
                uint64_t elem = *(docs_begin + i);
                elem = elem << score_bits_size;
                elem += *(score_begin + i);
                temp.push_back(elem);
            }

            if (!n)
                throw std::invalid_argument("List must be nonempty");
            succinct::bit_vector_builder docs_bits;
            write_gamma_nonzero(docs_bits, n);
            Sequence::write(docs_bits, temp.begin(),
                m_num_docs, n,
                m_params);
            m_docs_sequences.append(docs_bits);
        }

        void build(bitvector_collection& docs_sequences)
        {
            m_docs_sequences.build(docs_sequences);
        }

        global_parameters params()
        {
            return m_params;
        }

        uint64_t num_docs()
        {
            return m_num_docs;
        }

    private:
        global_parameters m_params;
        uint64_t m_num_docs;
        bitvector_collection::builder m_docs_sequences;
    };

    static float inline score(uint32_t index)
    {
        const float quant = 1.f / configuration::get().reference_size;
        return quant * (index + 1);
    }
};

template <typename score_compressor = uniform_score_compressor>
class wand_data_compressed {
public:
    class builder {
    public:
        builder(partition_type type, binary_freq_collection const& coll, global_parameters const& params)
            : total_elements(0)
            , total_blocks(0)
            , effective_list(0)
            , type(type)
            , params(params)
            , compressor_builder(coll.num_docs(), params)
        {
            logger() << "Storing max weight for each list and for each block..." << std::endl;
        }

        std::pair<float,float> 
        add_sequence(binary_freq_collection::sequence const& seq, std::vector<float> const& norm_lens,
                     const uint32_t term_ctf, std::unique_ptr<doc_scorer>& ranker)
        {

            // JMM: Always use wand, do not allow non-wand lists
            auto t = ((type == partition_type::fixed_blocks) ? static_block_partition(seq, norm_lens, term_ctf, ranker)
                                                             : variable_block_partition(seq, norm_lens, term_ctf, ranker));

            auto ind = compressor_builder.compress_data(std::get<2>(t));

            // JMM: No compression of the doc weights for now... Maybe we can look at this later

            compressor_builder.add_posting_list(std::get<1>(t).size(), std::get<1>(t).begin(),
                ind.begin());

            max_term_weight.push_back(*(std::max_element(std::get<2>(t).begin(), std::get<2>(t).end())));
            //max_document_weight.push_back(*(std::max_element(std::get<3>(t).begin(), std::get<3>(t).end())));
            total_elements += seq.docs.size();
            total_blocks += std::get<1>(t).size();
            effective_list++;
            return std::make_pair(max_term_weight.back(), std::numeric_limits<float>::lowest());
        }

        void build(wand_data_compressed& wdata)
        {

            wdata.m_num_docs = compressor_builder.num_docs();
            wdata.m_params = compressor_builder.params();
            compressor_builder.build(wdata.m_docs_sequences);
            logger() << "number of elements / number of blocks: " << (float)total_elements / (float)total_blocks << std::endl;
        }

        uint64_t total_elements;
        uint64_t total_blocks;
        uint64_t effective_list;
        partition_type type;
        std::vector<float> score_references;
        std::vector<float> max_term_weight;
        global_parameters const& params;
        typename score_compressor::builder compressor_builder;
    };

    class enumerator {
        friend class wand_data_compressed;

    public:
        enumerator(compact_elias_fano::enumerator docs_enum)
            : m_docs_enum(docs_enum)
        {
            reset();
        }

        void reset()
        {
            uint64_t val = m_docs_enum.move(0).second;
            m_cur_docid = val >> score_bits_size;
            uint64_t mask = configuration::get().reference_size - 1;
            m_cur_score_index = (val & mask);
        }

        void DS2I_FLATTEN_FUNC next_geq(uint64_t lower_bound)
        {
            if (docid() != lower_bound) {
                lower_bound = lower_bound << score_bits_size;
                auto val = m_docs_enum.next_geq(lower_bound);
                m_cur_docid = val.second >> score_bits_size;
                uint64_t mask = configuration::get().reference_size - 1;
                m_cur_score_index = (val.second & mask);
            }
        }

        void DS2I_FLATTEN_FUNC next()
        {
            std::cerr << "please implement compressed wand data iterator::next()";
            exit(EXIT_FAILURE);
        }

        uint64_t DS2I_FLATTEN_FUNC size()
        {
            std::cerr << "please implement compressed wand data iterator::size()";
            exit(EXIT_FAILURE);
            return 0;
        }

        float DS2I_FLATTEN_FUNC score()
        {
            return score_compressor::score(m_cur_score_index);
        }

        // hardcoded zero return until implemented
        float DS2I_FLATTEN_FUNC doc_weight() const
        {
            return 0.0f;
        }
 
        uint64_t DS2I_FLATTEN_FUNC docid() const
        {
            return m_cur_docid;
        }

    private:
        uint64_t m_cur_docid;
        uint64_t m_cur_score_index;
        compact_elias_fano::enumerator m_docs_enum;
    };

    uint64_t size() const
    {
        return m_docs_sequences.size();
    }

    uint64_t num_docs() const
    {
        return m_num_docs;
    }

    enumerator get_enum(size_t i) const
    {
        assert(i < size());
        auto docs_it = m_docs_sequences.get(m_params, i);

        uint64_t n = read_gamma_nonzero(docs_it);
        typename compact_elias_fano::enumerator docs_enum(m_docs_sequences.bits(),
            docs_it.position(),
            num_docs(), n,
            m_params);

        return enumerator(docs_enum);
    }

    template <typename Visitor>
    void map(Visitor& visit)
    {
        visit(m_params, "m_params")(m_num_docs, "m_num_docs")(m_docs_sequences, "m_docs_sequences");
            // (m_block_max_document_weight, "m_block_max_document_weight");
    }

private:
    global_parameters m_params;
    uint64_t m_num_docs;
    bitvector_collection m_docs_sequences;
    //succinct::mapper::mappable_vector<float> m_block_max_document_weight;
 
};
}
