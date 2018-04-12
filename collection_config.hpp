#pragma once

#include "util.hpp"


// Store collection data
struct collection_config {

  collection_config(std::ifstream& configuration) {

    std::string line;
    while(std::getline(configuration, line)) {
        auto delim = line.find("=");
        std::string variable = line.substr(0, delim);
        std::string value = line.substr(delim);
        
        if (variable == "raw_collection") {
            m_lexicon_file = value + ".lexicon";
            m_map_file = value + ".docids";
        }
        else if (variable == "inverted_index") {
            m_invidx_file = value;
        }
        else if (variable == "forward_index") {
            m_forward_index = value;
        }
        else if (variable == "wand_file") {
            m_wand_file = value;
        }
        else if (variable == "docs_to_expand") {
            m_docs_to_expand = std::stoull(value);
        }
        else if (variable == "terms_to_expand") {
            m_terms_to_expand = std::stoull(value);
        }
        else if (variable == "lambda_expand") {
            m_lambda = std::stof(value); 
        }
        else if (variable == "final_k") {
            m_final_k = std::stoull(value);
        }
        else {
            std::cerr << "Cannot parse parameter. Exiting." << std::endl;
            exit(EXIT_FAILURE);
        }
    }

  }
  std::string m_lexicon_file = ""; //raw_collection.lexicon
  std::string m_map_file = ""; //raw_collection.docids
  std::string m_invidx_file = "";
  std::string m_fidx_file = "";
  std::string m_wand_file = "";
  uint64_t m_docs_to_expand = 0;
  uint64_t m_terms_to_expand = 0;
  double m_lambda = 0;
  uint64_t m_final_k = 0;
};


/* Sample file
--------------
raw_collection=path/to/raw/collection
inverted_index=path/to/invidx
forward_index=path/to/forwardidx
wand_file=path/to/blah
docs_to_expand=25
terms_to_expand=25
lambda_expand=0.1
final_k=1000
--------------
*/
