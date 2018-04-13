#pragma once

#include <iostream>
#include <sstream>

#include "index_types.hpp"
#include "wand_data_compressed.hpp"
#include "util.hpp"
#include "wand_data_raw.hpp"
#include "wand_data.hpp"
#include "queries_util.hpp"
#include <math.h>

namespace ds2i {


    template <typename WandType>
    struct wand_query {

        wand_query(WandType const &wdata, uint64_t k = 10)
                : m_wdata(&wdata), m_topk(k) { 

        }

        template<typename Index>
        std::pair<uint64_t, uint64_t> operator()(Index const &index, term_id_vec const &terms,
                                                  std::unique_ptr<doc_scorer>& ranker) {
        
            m_topk.clear();
            if (terms.empty()) return {0, 0};

            size_t PROFILE_unique_pivots = 0;
            size_t PROFILE_postings_scored = 0;
            const size_t q_len = terms.size();
 
            auto query_term_freqs = query_freqs(terms);

            uint64_t num_docs = index.num_docs();
            typedef typename Index::document_enumerator enum_type;
            struct scored_enum {
                enum_type docs_enum;
                double q_weight;
                double max_term_weight; // list max score
                double max_document_weight; // lmds
                double term_ctf;
            };

            std::vector<scored_enum> enums;
            enums.reserve(query_term_freqs.size());

            for (auto term: query_term_freqs) {
                std::cerr << "index size = " << index.size() << "\n";
                auto list = index[term.first];
                auto q_weight = ranker->query_term_weight
                        (term.second, list.size());
                auto max_weight = q_weight * m_wdata->max_term_weight(term.first);
                auto max_static_weight = m_wdata->max_document_weight(term.first);
                double term_ctf = m_wdata->ctf(term.first); // JMM: We now have ctf
                enums.push_back(
                  scored_enum { 
                          std::move(list), 
                          q_weight, 
                          max_weight, 
                          max_static_weight,
                          term_ctf 
                  }
                );
            }


            std::vector<scored_enum *> ordered_enums;
            ordered_enums.reserve(enums.size());
            for (auto &en: enums) {
                ordered_enums.push_back(&en);
            }

            auto sort_enums = [&]() {
                // sort enumerators by increasing docid
                std::sort(ordered_enums.begin(), ordered_enums.end(),
                          [](scored_enum *lhs, scored_enum *rhs) {
                              return lhs->docs_enum.docid() < rhs->docs_enum.docid();
                          });
            };

            sort_enums();
            while (true) {
                // find pivot
                double upper_bound = 0;
                double max_static_score = std::numeric_limits<double>::lowest();
                size_t pivot;
                bool found_pivot = false;
                for (pivot = 0; pivot < ordered_enums.size(); ++pivot) {
                    if (ordered_enums[pivot]->docs_enum.docid() == num_docs) {
                        break;
                    }
                    max_static_score = std::max(max_static_score, 
                                                ordered_enums[pivot]->max_document_weight);
                    upper_bound += ordered_enums[pivot]->max_term_weight;

                    if (m_topk.would_enter((q_len * max_static_score) + upper_bound)) {
                        found_pivot = true;
                        break;
                    }
                }

                // no pivot found, we can stop the search
                if (!found_pivot) {
                    break;
                }

                // check if pivot is a possible match
                uint64_t pivot_id = ordered_enums[pivot]->docs_enum.docid();
                if (pivot_id == ordered_enums[0]->docs_enum.docid()) {
                    ++PROFILE_unique_pivots;
                    double norm_len = m_wdata->norm_len(pivot_id);
                    double score = ranker->calculate_document_weight(norm_len) * q_len;
                    for (scored_enum *en: ordered_enums) {
                        if (en->docs_enum.docid() != pivot_id) {
                            break;
                        }
                        ++PROFILE_postings_scored;
                        score += en->q_weight * ranker->doc_term_weight
                                (en->docs_enum.freq(), norm_len, en->term_ctf);
                        en->docs_enum.next();
                    }

                    m_topk.insert(score, pivot_id);
                    // resort by docid
                    sort_enums();
                } else {
                    // no match, move farthest list up to the pivot
                    uint64_t next_list = pivot;
                    for (; ordered_enums[next_list]->docs_enum.docid() == pivot_id;
                           --next_list);
                    ordered_enums[next_list]->docs_enum.next_geq(pivot_id);
                    // bubble down the advanced list
                    for (size_t i = next_list + 1; i < ordered_enums.size(); ++i) {
                        if (ordered_enums[i]->docs_enum.docid() <
                            ordered_enums[i - 1]->docs_enum.docid()) {
                            std::swap(ordered_enums[i], ordered_enums[i - 1]);
                        } else {
                            break;
                        }
                    }
                }
            }

            m_topk.finalize();
            return {PROFILE_unique_pivots, PROFILE_postings_scored};
        }

        std::vector<std::pair<double, uint64_t>> const &topk() const {
            return m_topk.topk();
        }

    private:
        WandType const *m_wdata;
        topk_queue m_topk;
    };

    
    template <typename WandType>
    struct block_max_wand_query {

        block_max_wand_query(WandType const &wdata, uint64_t k = 10)
                : m_wdata(&wdata), m_topk(k) {
        }


        template<typename Index>
        std::pair<uint64_t, uint64_t> operator()(Index const &index, term_id_vec const &terms,
                                                 std::unique_ptr<doc_scorer>& ranker) {
            
            m_topk.clear();
            if (terms.empty()) return {0,0};

            size_t PROFILE_unique_pivots = 0;
            size_t PROFILE_postings_scored = 0;
            const size_t q_len = terms.size();
 
            auto query_term_freqs = query_freqs(terms);

            uint64_t num_docs = index.num_docs();
            typedef typename Index::document_enumerator enum_type;
            typedef typename WandType::wand_data_enumerator wdata_enum;

            struct scored_enum {
                enum_type docs_enum;
                wdata_enum w;
                double q_weight;
                double max_term_weight; // list max score
                double max_document_weight; // lmds static weight
                double term_ctf;
            };

            std::vector<scored_enum> enums;
            enums.reserve(query_term_freqs.size());

            for (auto term: query_term_freqs) {
                auto list = index[term.first];
                auto w_enum = m_wdata->getenum(term.first);
                auto q_weight = ranker->query_term_weight
                        (term.second, list.size());
                double max_weight = q_weight * m_wdata->max_term_weight(term.first);
                double max_static_weight = m_wdata->max_document_weight(term.first);
                double term_ctf = m_wdata->ctf(term.first);
                enums.push_back(
                  scored_enum{ 
                          std::move(list), 
                          w_enum, 
                          q_weight, 
                          max_weight,
                          max_static_weight,
                          term_ctf
                   }
                );
            }

            std::vector<scored_enum *> ordered_enums;
            ordered_enums.reserve(enums.size());
            for (auto &en: enums) {
                ordered_enums.push_back(&en);
            }


            auto sort_enums = [&]() {
                // sort enumerators by increasing docid
                std::sort(ordered_enums.begin(), ordered_enums.end(),
                          [](scored_enum *lhs, scored_enum *rhs) {
                              return lhs->docs_enum.docid() < rhs->docs_enum.docid();
                          });
            };

            sort_enums();
            while (true) {

                // find pivot
                double upper_bound = 0.f;
                double max_static_score = std::numeric_limits<double>::lowest();
                size_t pivot;
                bool found_pivot = false;
                uint64_t pivot_id = num_docs;
                for (pivot = 0; pivot < ordered_enums.size(); ++pivot) {
                    if (ordered_enums[pivot]->docs_enum.docid() == num_docs) {
                        break;
                    }
                    max_static_score = std::max(max_static_score,
                                                ordered_enums[pivot]->max_document_weight);
                    upper_bound += ordered_enums[pivot]->max_term_weight;
                    if (m_topk.would_enter((q_len * max_static_score) + upper_bound)) {
                        found_pivot = true;
                        pivot_id = ordered_enums[pivot]->docs_enum.docid();
                        for (; pivot + 1 < ordered_enums.size() &&
                               ordered_enums[pivot + 1]->docs_enum.docid() == pivot_id; ++pivot);
                        break;
                    }
                }

                // no pivot found, we can stop the search
                if (!found_pivot) {
                    break;
                }

                double block_upper_bound = 0;
                double block_static_upper_bound = std::numeric_limits<double>::lowest();
                for (size_t i = 0; i < pivot + 1; ++i) {
                    if (ordered_enums[i]->w.docid() < pivot_id) {
                        ordered_enums[i]->w.next_geq(pivot_id);
                    }
                    block_upper_bound += ordered_enums[i]->w.score() * 
                                         ordered_enums[i]->q_weight;
                    block_static_upper_bound = std::max(block_static_upper_bound,
                                                        ordered_enums[i]->max_document_weight);
                }
                
              
                // We must use the static block UB instead of the true pivot one, otherwise we will
                // be block skipping and will miss potentially relevant documents 
                if (m_topk.would_enter(block_upper_bound + (block_static_upper_bound * q_len))) {


                    // check if pivot is a possible match
                    if (pivot_id == ordered_enums[0]->docs_enum.docid()) {
                        ++PROFILE_unique_pivots;
                        
                        // Set score to the documents true static weight
                        double norm_len = m_wdata->norm_len(pivot_id);
                        double score = q_len * ranker->calculate_document_weight(norm_len);
                        
                        // Update our 'max estimate' with the true doc-length norm:
                        // this tightens the bound (score is a negative number here)
                        block_upper_bound += score;

                        for (scored_enum *en: ordered_enums) {
                            if (en->docs_enum.docid() != pivot_id) {
                                break;
                            }
                            ++PROFILE_postings_scored;
                            double part_score = en->q_weight * ranker->doc_term_weight
                                    (en->docs_enum.freq(), norm_len, en->term_ctf);
                            score += part_score;
                            // Tighten the bounds for each score contribution
                            block_upper_bound -= en->w.score() * en->q_weight - part_score;
                            if (!m_topk.would_enter(block_upper_bound)) {
                                break;
                            }

                        }
                        for (scored_enum *en: ordered_enums) {
                            if (en->docs_enum.docid() != pivot_id) {
                                break;
                            }
                            en->docs_enum.next();
                        }

                        m_topk.insert(score, pivot_id);
                        // resort by docid
                        sort_enums();

                    } else {

                        uint64_t next_list = pivot;
                        for (; ordered_enums[next_list]->docs_enum.docid() == pivot_id;
                               --next_list);
                        ordered_enums[next_list]->docs_enum.next_geq(pivot_id);

                        // bubble down the advanced list
                        for (size_t i = next_list + 1; i < ordered_enums.size(); ++i) {
                            if (ordered_enums[i]->docs_enum.docid() <=
                                ordered_enums[i - 1]->docs_enum.docid()) {
                                std::swap(ordered_enums[i], ordered_enums[i - 1]);
                            } else {
                                break;
                            }
                        }
                    }

                } 
                // BM SKip block
                else {


                    uint64_t next;
                    uint64_t next_list = pivot;

                    double q_weight = ordered_enums[next_list]->q_weight;


                    for (uint64_t i = 0; i < pivot; i++){
                        if (ordered_enums[i]->q_weight > q_weight){
                            next_list = i;
                            q_weight = ordered_enums[i]->q_weight;
                        }
                    }

                    // TO BE FIXED (change with num_docs())
                    uint64_t next_jump = uint64_t(-2);

                    if (pivot + 1 < ordered_enums.size()) {
                        next_jump = ordered_enums[pivot + 1]->docs_enum.docid();
                    }


                    for (size_t i = 0; i <= pivot; ++i){
                        if (ordered_enums[i]->w.docid() < next_jump)
                            next_jump = std::min(ordered_enums[i]->w.docid(), next_jump);
                    }

                    next = next_jump + 1;
                    if (pivot + 1 < ordered_enums.size()) {
                        if (next > ordered_enums[pivot + 1]->docs_enum.docid()) {
                            next = ordered_enums[pivot + 1]->docs_enum.docid();
                        }
                    }

                    if (next <= ordered_enums[pivot]->docs_enum.docid()) {
                        next = ordered_enums[pivot]->docs_enum.docid() + 1;
                    }

                    ordered_enums[next_list]->docs_enum.next_geq(next);

                    // bubble down the advanced list
                    for (size_t i = next_list + 1; i < ordered_enums.size(); ++i) {
                        if (ordered_enums[i]->docs_enum.docid() <
                            ordered_enums[i - 1]->docs_enum.docid()) {
                            std::swap(ordered_enums[i], ordered_enums[i - 1]);
                        } else {
                            break;
                        }
                    }
                }
            }


            m_topk.finalize();
            return {PROFILE_unique_pivots, PROFILE_postings_scored};
        }


        std::vector<std::pair<double, uint64_t>> const &topk() const {
            return m_topk.topk();
        }

        void clear_topk() {
            m_topk.clear();
        }

        topk_queue const &get_topk() const {
            return m_topk;
        }

    private:

        WandType const *m_wdata;
        topk_queue m_topk;
    };


template <typename WandType>
    struct ranked_or_query {


        ranked_or_query(WandType const &wdata, uint64_t k = 10) 
                : m_wdata(&wdata), m_topk(k) { }

        template<typename Index>
        std::pair<uint64_t, uint64_t> operator()(Index const &index, term_id_vec terms,
                                                std::unique_ptr<doc_scorer>& ranker) {

            m_topk.clear();
            if (terms.empty()) return {0,0};

            size_t PROFILE_unique_pivots = 0;
            size_t PROFILE_postings_scored = 0;

            const size_t q_len = terms.size();
            auto query_term_freqs = query_freqs(terms);

            uint64_t num_docs = index.num_docs();
            typedef typename Index::document_enumerator enum_type;
            struct scored_enum {
                enum_type docs_enum;
                double q_weight;
                double term_ctf;
            };

            std::vector<scored_enum> enums;
            enums.reserve(query_term_freqs.size());

            for (auto term: query_term_freqs) {
                auto list = index[term.first];
                double ctf = m_wdata->ctf(term.first);
                auto q_weight = ranker->query_term_weight
                        (term.second, list.size());
                enums.push_back(scored_enum {std::move(list), q_weight, ctf});
            }

            uint64_t cur_doc =
                    std::min_element(enums.begin(), enums.end(),
                                     [](scored_enum const &lhs, scored_enum const &rhs) {
                                         return lhs.docs_enum.docid() < rhs.docs_enum.docid();
                                     })
                            ->docs_enum.docid();

            while (cur_doc < num_docs) {
                ++PROFILE_unique_pivots;
                double norm_len = m_wdata->norm_len(cur_doc);
                double score = ranker->calculate_document_weight(norm_len) * q_len;
                uint64_t next_doc = index.num_docs();
                for (size_t i = 0; i < enums.size(); ++i) {
                    if (enums[i].docs_enum.docid() == cur_doc) {
                        ++PROFILE_postings_scored;
                        score += enums[i].q_weight * ranker->doc_term_weight
                                (enums[i].docs_enum.freq(), norm_len, enums[i].term_ctf);
                        enums[i].docs_enum.next();
                    }
                    if (enums[i].docs_enum.docid() < next_doc) {
                        next_doc = enums[i].docs_enum.docid();
                    }
                }
                m_topk.insert(score, cur_doc);
                cur_doc = next_doc;
            }

            m_topk.finalize();
            return {PROFILE_unique_pivots, PROFILE_postings_scored};
 
        }

        std::vector<std::pair<double, uint64_t> > const &topk() const {
            return m_topk.topk();
        }

    private:
        WandType const *m_wdata;
        topk_queue m_topk;
    };


    template <typename WandType>
    struct maxscore_query {

        maxscore_query(WandType const &wdata, uint64_t k = 10)
                : m_wdata(&wdata), m_topk(k) {
        }

        template<typename Index>
        std::pair<uint64_t, uint64_t> operator()(Index const &index, term_id_vec const &terms,
                                                std::unique_ptr<doc_scorer>& ranker) {

            m_topk.clear();
            if (terms.empty()) return {0,0};

            size_t PROFILE_unique_pivots = 0;
            size_t PROFILE_postings_scored = 0;
            const size_t q_len = terms.size(); 

            auto query_term_freqs = query_freqs(terms);

            uint64_t num_docs = index.num_docs();
            typedef typename Index::document_enumerator enum_type;
            struct scored_enum {
                enum_type docs_enum;
                double q_weight;
                double max_term_weight; // list max ub
                double max_document_weight; // lmds static score
                double term_ctf;
            };

            std::vector<scored_enum> enums;
            enums.reserve(query_term_freqs.size());

            for (auto term: query_term_freqs) {
                auto list = index[term.first];
                auto q_weight = ranker->query_term_weight
                        (term.second, list.size());
                auto max_weight = q_weight * m_wdata->max_term_weight(term.first);
                auto max_static_weight = m_wdata->max_document_weight(term.first);
                double term_ctf = m_wdata->ctf(term.first);
                enums.push_back(
                  scored_enum {
                          std::move(list), 
                          q_weight, 
                          max_weight,
                          max_static_weight,
                          term_ctf
                  }
                );
            }

            std::vector<scored_enum *> ordered_enums;
            ordered_enums.reserve(enums.size());
            for (auto &en: enums) {
                ordered_enums.push_back(&en);
            }

            // sort enumerators by increasing maxscore
            std::sort(ordered_enums.begin(), ordered_enums.end(),
                      [](scored_enum *lhs, scored_enum *rhs) {
                          return lhs->max_term_weight < rhs->max_term_weight;
                      });

            std::vector<double> upper_bounds(ordered_enums.size());
            std::vector<double> doc_weight_bounds(ordered_enums.size());
            double max_static_weight = std::numeric_limits<double>::lowest();
            upper_bounds[0] = ordered_enums[0]->max_term_weight;
            max_static_weight = std::max(max_static_weight, ordered_enums[0]->max_document_weight);
            doc_weight_bounds[0] = max_static_weight * q_len;
            for (size_t i = 1; i < ordered_enums.size(); ++i) {
                upper_bounds[i] = upper_bounds[i - 1] + ordered_enums[i]->max_term_weight;
                max_static_weight = std::max(max_static_weight, 
                                             ordered_enums[i]->max_document_weight); 
                doc_weight_bounds[i] = max_static_weight * q_len;
            }

            uint64_t non_essential_lists = 0;
            uint64_t cur_doc =
                    std::min_element(enums.begin(), enums.end(),
                                     [](scored_enum const &lhs, scored_enum const &rhs) {
                                         return lhs.docs_enum.docid() < rhs.docs_enum.docid();
                                     })
                            ->docs_enum.docid();

            while (non_essential_lists < ordered_enums.size() &&
                   cur_doc < index.num_docs()) {
                ++PROFILE_unique_pivots;
                double norm_len = m_wdata->norm_len(cur_doc);
                double score = ranker->calculate_document_weight(norm_len) * q_len; 
                uint64_t next_doc = num_docs;
                for (size_t i = non_essential_lists; i < ordered_enums.size(); ++i) {
                    if (ordered_enums[i]->docs_enum.docid() == cur_doc) {
                        ++PROFILE_postings_scored;
                        score += ordered_enums[i]->q_weight * ranker->doc_term_weight
                                (ordered_enums[i]->docs_enum.freq(), norm_len, 
                                 ordered_enums[i]->term_ctf);
                        ordered_enums[i]->docs_enum.next();
                    }
                    if (ordered_enums[i]->docs_enum.docid() < next_doc) {
                        next_doc = ordered_enums[i]->docs_enum.docid();
                    }
                }

                // try to complete evaluation with non-essential lists
                for (size_t i = non_essential_lists - 1; i + 1 > 0; --i) {
                    if (!m_topk.would_enter(score + upper_bounds[i])) {
                        break;
                    }
                    ordered_enums[i]->docs_enum.next_geq(cur_doc);
                    if (ordered_enums[i]->docs_enum.docid() == cur_doc) {
                        ++PROFILE_postings_scored;
                        score += ordered_enums[i]->q_weight * ranker->doc_term_weight
                                (ordered_enums[i]->docs_enum.freq(), norm_len, 
                                 ordered_enums[i]->term_ctf);
                    }
                }

                if (m_topk.insert(score, cur_doc)) {
                    // update non-essential lists
                    while (non_essential_lists < ordered_enums.size() &&
                           !m_topk.would_enter(upper_bounds[non_essential_lists] +
                                               doc_weight_bounds[non_essential_lists])) {
                        non_essential_lists += 1;
                    }
                }

                cur_doc = next_doc;
            }

            m_topk.finalize();
            return {PROFILE_unique_pivots, PROFILE_postings_scored};
        }

        std::vector<std::pair<double, uint64_t>> const &topk() const {
            return m_topk.topk();
        }

    private:
        WandType const *m_wdata;
        topk_queue m_topk;
    }; 
}

