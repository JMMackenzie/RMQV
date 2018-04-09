#pragma once

#include "configuration.hpp"
#include "score_partitioning.hpp"
#include "global_parameters.hpp"

namespace ds2i {

enum class partition_type { fixed_blocks, variable_blocks };

std::tuple<std::vector<uint32_t>, std::vector<uint32_t>, std::vector<float>, std::vector<float>>
static_block_partition(binary_freq_collection::sequence const &seq,
                       std::vector<float> const &norm_lens,
                       uint32_t term_ctf,
                       std::unique_ptr<doc_scorer>& ranker){
 
  std::vector<uint32_t> sizes;
  std::vector<uint32_t> block_docid;
  std::vector<float> block_max_term_weight;
  std::vector<float> block_max_doc_weight;
  uint64_t block_size = configuration::get().block_size;

  // Auxiliary vector
  float block_max_score = 0;
  float block_max_weight = std::numeric_limits<float>::lowest();
  size_t current_block = 0;
  size_t i;

  for (i = 0; i < seq.docs.size(); ++i) {
    uint64_t docid = *(seq.docs.begin() + i);
    uint64_t freq = *(seq.freqs.begin() + i);
    float score = ranker->doc_term_weight(freq, norm_lens[docid], term_ctf);
    float weight = ranker->calculate_document_weight(norm_lens[docid]);
    if (i == 0 || (i / block_size) == current_block) {
      block_max_score = std::max(block_max_score, score);
      block_max_weight = std::max(block_max_weight, weight);
    } else {
      block_docid.push_back(*(seq.docs.begin() + i) - 1);
      block_max_term_weight.push_back(block_max_score);
      block_max_doc_weight.push_back(block_max_weight);
      current_block++;
      block_max_score = std::max((float)0, score);
      block_max_weight = std::max(std::numeric_limits<float>::lowest(), weight);
      sizes.push_back(block_size);
    }
  }
  block_docid.push_back(*(seq.docs.begin() + seq.docs.size() - 1));
  block_max_term_weight.push_back(block_max_score);
  block_max_doc_weight.push_back(block_max_weight);
  sizes.push_back((i % block_size) ? i % block_size : block_size);

  return std::make_tuple(sizes, block_docid, block_max_term_weight, block_max_doc_weight);
}

std::tuple<std::vector<uint32_t>, std::vector<uint32_t>, std::vector<float>, std::vector<float>>
variable_block_partition(binary_freq_collection::sequence const &seq,
                         std::vector<float> const &norm_lens,
                         uint32_t term_ctf,
                         std::unique_ptr<doc_scorer>& ranker){
 
                         
  auto eps1 = configuration::get().eps1_wand;
  auto eps2 = configuration::get().eps2_wand;
  auto fixed_cost = configuration::get().fixed_cost_wand_partition;

  // Auxiliary vector
  std::vector<std::tuple<uint64_t, float, bool>> doc_score_top;
  std::vector<float> doc_weights;
  float max_score = 0;

  for (size_t i = 0; i < seq.docs.size(); ++i) {
    uint64_t docid = *(seq.docs.begin() + i);
    uint64_t freq = *(seq.freqs.begin() + i);
    float score = ranker->doc_term_weight(freq, norm_lens[docid], term_ctf);
    float weight = ranker->calculate_document_weight(norm_lens[docid]);
    doc_weights.emplace_back(weight);
    doc_score_top.emplace_back(docid, score, false);
    max_score = std::max(max_score, score);
  }

  float estimated_idf = ranker->query_term_weight(1, seq.docs.size());
  auto p = score_opt_partition(doc_score_top.begin(), 0, doc_score_top.size(),
                               eps1, eps2, fixed_cost, estimated_idf, doc_weights);

  // Given a partition, compute the block-max weights

  return std::make_tuple(p.sizes, p.docids, p.max_values, p.max_weights);
}

}  // namespace ds2i
