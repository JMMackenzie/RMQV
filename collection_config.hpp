#pragma once


// Store collection data
struct collection_config {

  collection_config(std::ifstream& configuration) {


  }
  std::string m_lexicon_file = ""; //raw_collection.lexicon
  std::string m_map_file = ""; //raw_collection.docids
  std::string m_invidx_file = "";
  std::string m_fidx_file = "";
  std::string m_wand_file = "";
  uint64_t m_docs_to_expand = 0;
  uint64_t m_terms_to_expand = 0;
  double m_lambda = 0;
  uint64_t final_k = 0;
};


/* Sample file
--------------
raw_collection = path/to/raw/collection
inverted_index = path/to/invidx
forward_index = path/to/forwardidx
wand_file = path/to/blah
docs_to_expand = 25
terms_to_expand = 25
lambda_expand = 0.1
final_k = 1000
--------------
*/
