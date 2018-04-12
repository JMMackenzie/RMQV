#pragma once

#include "util.hpp"
#include "document_vector.hpp"

class document_index {
 
  private: 
    // m_doc_vectors[i] returns the document_vector for document i
    std::vector<document_vector> m_doc_vectors;
    uint32_t m_size;
    uint32_t no_terms;

   // Helper struct for our RM calculations
    struct vector_wrapper {
        typename document_vector::const_iterator cur;
        typename document_vector::const_iterator end;
        double doc_score;
        uint32_t doc_len;
        vector_wrapper() = default;
        vector_wrapper(document_vector& dv, double score) 
                      : doc_score(score) {
            cur = dv.begin();
            end = dv.end();
            doc_len = dv.doclen();
        } 
    };

  public:
    document_index() : m_size(0) {}

    // Build a document index from ds2i files
    document_index(std::string ds2i_basename, std::unordered_set<uint32_t>& stoplist) {
        // Temporary 'plain' index structures
        std::vector<std::vector<uint32_t>> plain_terms;
        std::vector<std::vector<uint32_t>> plain_freqs;

        // Read DS2i document file prefix
        std::ifstream docs (ds2i_basename + ".docs", std::ios::binary);
        std::ifstream freqs (ds2i_basename + ".freqs", std::ios::binary);

        // Check files are OK

        // Read first sequence from docs
        uint32_t one;
        docs.read(reinterpret_cast<char *>(&one), sizeof(uint32_t));
        docs.read(reinterpret_cast<char *>(&m_size), sizeof(uint32_t));
        m_doc_vectors.reserve(m_size); // Note: not yet constructed
        plain_terms.resize(m_size); 
        plain_freqs.resize(m_size);

        uint32_t d_seq_len = 0;
        uint32_t f_seq_len = 0;
        uint32_t term_id = 0;
        // Sequences are now aligned. Walk them.        
        while(!docs.eof() && !freqs.eof()) {

            // Check if the term is stopped
            bool stopped = (stoplist.find(term_id) != stoplist.end());


            docs.read(reinterpret_cast<char *>(&d_seq_len), sizeof(uint32_t));
            freqs.read(reinterpret_cast<char *>(&f_seq_len), sizeof(uint32_t));
            if (d_seq_len != f_seq_len) {
                std::cerr << "ERROR: Freq and Doc sequences are not aligned. Exiting."
                          << std::endl;
                exit(EXIT_FAILURE);
            }
            uint32_t seq_count = 0;
            uint32_t docid = 0;
            uint32_t fdt = 0;
            while (seq_count < d_seq_len) {
                docs.read(reinterpret_cast<char *>(&docid), sizeof(uint32_t));
                freqs.read(reinterpret_cast<char *>(&fdt), sizeof(uint32_t));
                // Only emplace unstopped terms
                if (!stopped) {
                    plain_terms[docid].emplace_back(term_id);
                    plain_freqs[docid].emplace_back(fdt);
                }
                ++seq_count;
            }
            ++term_id;
        }
        no_terms = term_id;
        
        std::cerr << "Read " << m_size << " lists and " << term_id 
                  << " unique terms. Compressing.\n";

        // Now iterate the plain index, compress, and store
        for (size_t i = 0; i < m_size; ++i) {
            m_doc_vectors.emplace_back(i, plain_terms[i], plain_freqs[i]);
        }

    } 

    void serialize(std::ostream& out) {
        out.write(reinterpret_cast<const char *>(&no_terms), sizeof(no_terms));
        out.write(reinterpret_cast<const char *>(&m_size), sizeof(m_size));
        for (size_t i = 0; i < m_size; ++i) {
            m_doc_vectors[i].serialize(out);
        }
    }

    void load(std::string inf) {
        std::ifstream in(inf, std::ios::binary);
        load(in);
    }

    void load(std::istream& in) {
        in.read(reinterpret_cast<char *>(&no_terms), sizeof(no_terms));
        in.read(reinterpret_cast<char *>(&m_size), sizeof(m_size));
        m_doc_vectors.resize(m_size);
        for (size_t i = 0; i < m_size; ++i) {
            m_doc_vectors[i].load(in);
        }
    }

    std::vector<std::pair<uint32_t, double>> 
    get_rm_indri(std::vector<vector_wrapper*>& docvectors) {

    } 


    // DaaT traversal for RM -- Non sort version
    std::vector<std::pair<uint32_t, double>> 
    get_rm_daat(std::vector<vector_wrapper*>& docvectors) {
    
        std::vector<std::pair<uint32_t, double>> result;    
        
        auto min = std::min_element(docvectors.begin(),
                                    docvectors.end(),
                                    [] (const vector_wrapper* lhs,
                                        const vector_wrapper* rhs) {
                                          return lhs->cur.termid() < rhs->cur.termid();
                                        });

        uint32_t cur_term = (*min)->cur.termid(); 
        // Main scoring loop
        while (cur_term < no_terms) {

            uint32_t next_term = no_terms;
            double score = 0;
            for (size_t i = 0; i < docvectors.size(); ++i) {
                if (docvectors[i]->cur.termid() == cur_term) {
                    score += docvectors[i]->doc_score * (docvectors[i]->cur.freq() / (docvectors[i]->doc_len * 1.0f));
                    docvectors[i]->cur.next();
                }
                if (docvectors[i]->cur.termid() < next_term) {
                    next_term = docvectors[i]->cur.termid();
                }
            }
            // Add result
            result.emplace_back(cur_term, score);
            cur_term = next_term;
        }  
        
        // Finalize results and return
        std::sort(result.begin(), result.end(), 
                  [](const std::pair<uint32_t, double> &lhs,
                     const std::pair<uint32_t, double> &rhs) {
                      return lhs.second > rhs.second;
                  });
        return result;
    } 



    std::vector<std::pair<uint32_t, double>>
    rm_expander (std::vector<std::pair<double, uint64_t>>& initial_retrieval,
                 size_t terms_to_expand = 0) {

        // 0. Result init
        std::vector<std::pair<uint32_t, double>> result;

        // 1. Prepare the document vectors
        std::vector<vector_wrapper> feedback_vectors(initial_retrieval.size());
        std::vector<vector_wrapper*> feedback_ptr;
        for (size_t i = 0; i < initial_retrieval.size(); ++i) {
            double score = initial_retrieval[i].first;
            uint64_t docid = initial_retrieval[i].second;
            feedback_vectors[i] = vector_wrapper(m_doc_vectors[docid], score);
            feedback_ptr.emplace_back(&(feedback_vectors[i]));
        }

        // Get the result and resize if needed
        result = get_rm_daat(feedback_ptr);
        if (terms_to_expand > 0)
          result.resize(terms_to_expand);
          
        return result;
        
    }

    

    void test_iteration(uint32_t docid) {
        
        document_vector::const_iterator cur = m_doc_vectors[docid].begin(); // Automatically decompresses
        document_vector::const_iterator end = m_doc_vectors[docid].end();
        
        while(cur != end) {
            std::cerr << cur.termid() << "," << cur.freq() << "\n";
            cur.next();
        }
        std::cerr << "Exiting here now.\n";
    }
};
