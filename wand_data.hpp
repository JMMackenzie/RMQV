#pragma once

#include "binary_freq_collection.hpp"
#include "rankers.hpp"
#include "succinct/mappable_vector.hpp"
#include "util.hpp"
#include "wand_data_raw.hpp"

class enumerator;
namespace ds2i {
    template<typename block_wand_type=wand_data_raw>
    class wand_data {
    public:

        using wand_data_enumerator = typename block_wand_type::enumerator;

        wand_data() { }

        template<typename LengthsIterator>
        wand_data(LengthsIterator len_it, uint64_t num_docs,
                      binary_freq_collection const &coll, partition_type type,
                      std::unique_ptr<doc_scorer>& ranker) {
            std::vector<float> max_term_weight;
            std::vector<float> max_document_weight;
            std::vector<uint32_t> ctf;
            std::vector<float> norm_lens(num_docs);
            size_t lens_sum = 0;
            double collection_size_in_terms = 0;
            bool compute_tf_idf_weights = false;
            global_parameters params;

            typename block_wand_type::builder builder(type, coll, params);

            // Collect the required stats
            //collect_global_index_statistics(len_it, num_docs, coll);

            logger() << "Calculating global index statistics..." << std::endl;
            // Read doc lengths
            for (size_t i = 0; i < num_docs; ++i) {
                float len = *len_it++;
                norm_lens[i] = len;
                lens_sum += len;
            }

            // Average document length
            float avg_len = float(lens_sum / double(num_docs));

            logger() << "Computing global term statistics now..." << std::endl;   
            
            // TF-IDF requires a document weight computation. We will do it here
            // if we see that the wand file is a TF-IDF one.
            if (ranker->id() == ranker_identifier::TFIDF)
              compute_tf_idf_weights = true;
 
            if (compute_tf_idf_weights) {
              std::cerr << "Computing TF/IDF doc weights\n";
              std::vector<float> document_weights (num_docs);
              for (auto const &seq: coll) {
                  uint32_t seq_ctf = 0;
                  for (size_t i = 0; i < seq.docs.size(); ++i) {
                      uint64_t doc = *(seq.docs.begin() + i);
                      uint64_t freq = *(seq.freqs.begin() + i);
                      float weight = 1 + std::log(freq);
                      weight = weight * weight;
                      document_weights[doc] += weight;
                      seq_ctf += freq;
                  }
                  ctf.push_back(seq_ctf);
                  collection_size_in_terms += seq_ctf;
              }
              // Now we need to take the square root of each weight and over-ride
              // the value in norm_lens
              for (size_t i = 0; i < document_weights.size(); ++i) {
                norm_lens[i] = 1.0 / std::sqrt(document_weights[i]); //TFIDF weight
              }
 
            }
            else {
              for (auto const &seq: coll) {
                  uint32_t seq_ctf = 0;
                  for (size_t i = 0; i < seq.docs.size(); ++i) {
                      uint64_t freq = *(seq.freqs.begin() + i);
                      seq_ctf += freq;
                  }
                  ctf.push_back(seq_ctf);
                  collection_size_in_terms += seq_ctf;
              }
            } 

            // Init new ranker now we have stats
            ranker->init(avg_len, num_docs, collection_size_in_terms);

            // Save the length as defined by the ranker
            for(auto& norm_len: norm_lens) {
                norm_len = ranker->norm_len(norm_len);
            }
            
            logger() << "Length calculations complete..." << std::endl;



            size_t cur_seq = 0;
            for (auto const &seq: coll) {
                uint32_t current_ctf = ctf[cur_seq];
                auto v = builder.add_sequence(seq, norm_lens, current_ctf, ranker);
                max_term_weight.push_back(v.first);
                max_document_weight.push_back(v.second);
                if ((max_term_weight.size() % 1000000) == 0) {
                    logger() << max_term_weight.size() << " list processed" << std::endl;
                }
                cur_seq++;
            }
            if ((max_term_weight.size() % 1000000) != 0) {
               logger() << max_term_weight.size() << " list processed" << std::endl;
            }

            builder.build(m_block_wand);
            m_max_term_weight.steal(max_term_weight);
            m_max_document_weight.steal(max_document_weight);
            m_norm_lens.steal(norm_lens);
            m_ctf.steal(ctf);
            m_avg_doclen = ranker->average_doc_len;
            m_num_docs = ranker->num_docs;
            m_terms_in_collection = ranker->total_terms_in_collection;
            m_ranker_id = ranker->id(); // tells us which ranker to use
            logger() << "Collection stats/scores computed." << std::endl;

        }


        float norm_len(uint64_t doc_id) const {
            return m_norm_lens[doc_id];
        }

        float max_term_weight(uint64_t list) const {
            return m_max_term_weight[list];
        }

        float max_document_weight(uint64_t list) const {
            return m_max_document_weight[list];
        }

        uint32_t ctf(uint64_t list) const {
            return m_ctf[list];
        }

        float average_doclen() const {
            return m_avg_doclen;
        }

        float num_docs() const {
            return m_num_docs;
        }

        double terms_in_collection() const {
            return m_terms_in_collection;
        }

        ranker_identifier ranker_id() const {
            return m_ranker_id;
        }

        wand_data_enumerator getenum(size_t i) const {
            return m_block_wand.get_enum(i);
        }

        template<typename Visitor>
        void map(Visitor &visit) {
            visit
                    (m_block_wand, "m_block_wand")
                    (m_norm_lens, "m_norm_lens")
                    (m_max_term_weight, "m_max_term_weight")
                    (m_max_document_weight, "m_max_document_weight")
                    (m_ctf, "m_ctf")
                    (m_avg_doclen, "m_avg_doclen")
                    (m_num_docs, "m_num_docs")
                    (m_terms_in_collection, "m_terms_in_collection")
                    (m_ranker_id, "m_ranker_id");
        }


    private:
        block_wand_type m_block_wand;
        succinct::mapper::mappable_vector<float> m_norm_lens;
        succinct::mapper::mappable_vector<float> m_max_term_weight;
        succinct::mapper::mappable_vector<float> m_max_document_weight;
        succinct::mapper::mappable_vector<uint32_t> m_ctf;
        float m_avg_doclen;
        float m_num_docs;
        double m_terms_in_collection;
        ranker_identifier m_ranker_id;
    };
}
