#pragma once

#include "util.hpp"
#include "compress_qmx.h" // QMX

//Implements a single document vector
class document_vector {

  public:
    using fast_vector = std::vector<uint32_t>;//, FastPForLib::cacheallocator>;
    using term_codec = ANT_compress_qmx; // We could use D4 but we do deltas ourselves
    using freq_codec = ANT_compress_qmx; 

    // Non-trivial constructor will initialize all values and compress
    // the input data
    document_vector(uint32_t docid, std::vector<uint32_t>& raw_terms, 
                    std::vector<uint32_t>& raw_freqs) : m_docid(docid), 
                    m_doclen(0), m_term_bytes(0), m_freq_bytes(0), m_size(0) {
 
        // Given our raw vectors, let's compress and store them
        compress_lists(raw_terms, raw_freqs);
    }

    // Trivial case used when loading
    document_vector() {}

  private:
    uint32_t m_docid; // Technically not required
    uint32_t m_doclen;
    std::vector<uint32_t> m_terms;
    std::vector<uint32_t> m_freqs;
   
    // Compression helpers
    uint64_t m_term_bytes;
    uint64_t m_freq_bytes;
    uint32_t m_size;   

   private:

    // Use QMX with deltas  because these are monotonic 
    void compress_terms(std::vector<uint32_t>& raw_terms) {

#ifdef PASSTHROUGH
        m_terms.resize(raw_terms.size());
        m_terms.assign(raw_terms.begin(), raw_terms.end());
        return;  
#endif

        // Compute deltas instead of the raw IDs
        fastDelta(raw_terms.data(), raw_terms.size());
        static term_codec term_compressor;
        m_terms.resize(2 * raw_terms.size() + 1024);
        uint32_t *terms_out = m_terms.data();
        term_compressor.encodeArray(raw_terms.data(), raw_terms.size(),
                                    terms_out, &m_term_bytes);
       
        uint32_t end_size = (m_term_bytes / sizeof(uint32_t));

        // Ensure we don't truncate any partial bytes
			  if (m_term_bytes % sizeof(uint32_t) != 0){
				    end_size++;
			  }
        
        if (end_size > m_terms.size()) {
            std::cerr << "Ran out of room while encoding terms.\n";
            exit(EXIT_FAILURE);
        }

        m_terms.resize(end_size);
        m_terms.shrink_to_fit();
    }

    // Use plain QMX here, not monotonic
    void compress_frequencies(std::vector<uint32_t>& raw_freqs) {

#ifdef PASSTHROUGH
        m_freqs.resize(raw_freqs.size());
        m_freqs.assign(raw_freqs.begin(), raw_freqs.end());
        return;
#endif 
        // Compute and store doc length
        m_doclen = std::accumulate(raw_freqs.begin(), raw_freqs.end(), 0);

        static freq_codec freq_compressor;
        m_freqs.resize(2 * raw_freqs.size() + 1024);
        uint32_t *freqs_out = m_freqs.data();
        freq_compressor.encodeArray(raw_freqs.data(), raw_freqs.size(),
                                    freqs_out, &m_freq_bytes);
        uint32_t end_size = (m_freq_bytes / sizeof(uint32_t));

			  if (m_freq_bytes % sizeof(uint32_t) != 0){
				    end_size++;
			  }

        if (end_size > m_freqs.size()) {
            std::cerr << "Ran out of room while encoding frequencies.\n";
            exit(EXIT_FAILURE);
        }

        m_freqs.resize(end_size);
        m_freqs.shrink_to_fit();
    }

    // Compression wrapper, does initial validation of input vectors
    void compress_lists(std::vector<uint32_t>& raw_terms,
                        std::vector<uint32_t>& raw_freqs) {

        if (raw_terms.size() != raw_freqs.size()) {
            std::cerr << "ERROR: Frequencies and Term vectors"
                      << " have differing sizes." << std::endl;
            exit(EXIT_FAILURE);
        }
        // Handle empty documents case
        if (raw_terms.size() == 0) {
            return;
        }
        m_size = raw_terms.size();
        compress_terms(raw_terms);
        compress_frequencies(raw_freqs);
    }



    void decompress_terms(fast_vector& target) const {

#ifdef PASSTHROUGH
        target.resize(m_terms.size());
        target.assign(m_terms.begin(), m_terms.end());
        return;
#endif

        static term_codec term_compressor;
        const uint32_t *term_start = m_terms.data();
        term_compressor.decodeArray(term_start, m_term_bytes, target.data(), m_size);
	
        // Un-delta our gaps	
        /* Extracted from: https:github.com/lemire/FastDifferentialCoding */
		    __m128i prev = _mm_set1_epi32(0);
		    size_t i = 0;
		    for (; i  < m_size/4; i++) {
			      __m128i curr = _mm_lddqu_si128((const __m128i *)target.data() + i);
			      const __m128i _tmp1 = _mm_add_epi32(_mm_slli_si128(curr, 8), curr);
			      const __m128i _tmp2 = _mm_add_epi32(_mm_slli_si128(_tmp1, 4), _tmp1);
			      prev = _mm_add_epi32(_tmp2, _mm_shuffle_epi32(prev, 0xff));
			      _mm_storeu_si128((__m128i *)target.data() + i,prev);
		    }
		    uint32_t lastprev = _mm_extract_epi32(prev, 3);
		    for(i = 4 * i ; i < m_size; ++i) {
			        lastprev = lastprev + target[i];
			        target[i] = lastprev;
		    }

    }

    void decompress_frequencies(fast_vector& target) const {

 #ifdef PASSTHROUGH
        target.resize(m_freqs.size());
        target.assign(m_freqs.begin(), m_freqs.end());
        return;
#endif
 
        static freq_codec freq_compressor;
        const uint32_t *freq_start = m_freqs.data();
        freq_compressor.decodeArray(freq_start, m_freq_bytes, target.data(), m_size);
    }

  public:

    // Public decompression call, used by iterator to retrieve real data
    void decompress_lists(fast_vector& terms, fast_vector& freqs) const {
        // Empty document, no decompression required
        if (m_size == 0) {
            return;
        }
        // Resize targets
        terms.resize(m_size);
        freqs.resize(m_size);
        decompress_terms(terms);
        decompress_frequencies(freqs); 
    }

    // Ugly freeze
    void serialize(std::ostream& out) {
        uint32_t tsize = m_terms.size();
        uint32_t fsize = m_freqs.size();
        out.write(reinterpret_cast<const char *>(&m_docid), sizeof(uint32_t));
        out.write(reinterpret_cast<const char *>(&m_doclen), sizeof(uint32_t));
        
        out.write(reinterpret_cast<const char *>(&m_term_bytes), sizeof(uint64_t));
        out.write(reinterpret_cast<const char *>(&m_freq_bytes), sizeof(uint64_t));
        out.write(reinterpret_cast<const char *>(&m_size), sizeof(uint32_t));
        out.write(reinterpret_cast<const char *>(&tsize), sizeof(uint32_t));
        out.write(reinterpret_cast<const char *>(&m_terms[0]), m_terms.size() * sizeof(uint32_t));
        out.write(reinterpret_cast<const char *>(&fsize), sizeof(uint32_t));
        out.write(reinterpret_cast<const char *>(&m_freqs[0]), m_freqs.size() * sizeof(uint32_t));
    }
   
    // Ugly load 
    void load(std::istream& in) {
        uint32_t tsize = 0;
        uint32_t fsize = 0;
        in.read(reinterpret_cast<char *>(&m_docid), sizeof(uint32_t));
        in.read(reinterpret_cast<char *>(&m_doclen), sizeof(uint32_t));
        in.read(reinterpret_cast<char *>(&m_term_bytes), sizeof(uint64_t));
        in.read(reinterpret_cast<char *>(&m_freq_bytes), sizeof(uint64_t));
        in.read(reinterpret_cast<char *>(&m_size), sizeof(uint32_t));
        in.read(reinterpret_cast<char *>(&tsize), sizeof(uint32_t));
        m_terms.resize(tsize);
        in.read(reinterpret_cast<char *>(&m_terms[0]), m_terms.size() * sizeof(uint32_t));
        in.read(reinterpret_cast<char *>(&fsize), sizeof(uint32_t));
        m_freqs.resize(fsize);
        in.read(reinterpret_cast<char *>(&m_freqs[0]), m_freqs.size() * sizeof(uint32_t));
    }
  
    uint32_t size() const {
        return m_size;
    }

    uint32_t doclen() const {
        return m_doclen;
    }

    // Iterator helper -- used for traversal
    class vector_iterator {

      private:
        const document_vector *m_vector_ptr = nullptr;
        uint32_t m_cur_pos;
        uint32_t m_size;
        mutable uint32_t m_cur_term;
        mutable uint32_t m_cur_freq;
        mutable std::vector<uint32_t> m_decoded_terms; 
        mutable std::vector<uint32_t> m_decoded_freqs; 

        // Only allow private -- called on initializing a new vector with
        // pos 0 ie, begin()
        void init () { 
            // Decompress all at once and prepare for reading
            m_vector_ptr->decompress_lists(m_decoded_terms, m_decoded_freqs);
            // Add a dummy id to the end of the lists
            m_decoded_terms.push_back(-1);
            m_decoded_freqs.push_back(-1);
        }

      public:

        // Explicit default constructor
        vector_iterator() = default;

        // Give a pointer to the doc vector
        vector_iterator(const document_vector& dv, uint32_t pos) {
            m_cur_pos = pos;
            m_vector_ptr = &dv;
            m_size = m_vector_ptr->size() + 1;
            if (pos == 0) 
              init();
        }

        void reset() {
            m_cur_pos = 0;
        }
     
        uint32_t position() const {
            return m_cur_pos;
        }

        uint32_t size() const {
            return m_size;
        }

        uint32_t doclen() const {
            return m_vector_ptr->doclen();
        }

        void next() {
            if(m_cur_pos != size()) {
                ++m_cur_pos;
            }
            else {
              throw std::out_of_range("Vector iterator dereferenced at list end.");
            }
        } 

        uint32_t termid() const {
            if (m_cur_pos == size())
              throw std::out_of_range("Vector iterator dereferenced at list end.");
            m_cur_term = m_decoded_terms[m_cur_pos];
            return m_cur_term;
        }

        uint32_t freq() const {
            if (m_cur_pos == size())
              throw std::out_of_range("Vector iterator dereferenced at list end.");
            m_cur_freq = m_decoded_freqs[m_cur_pos];
            return m_cur_freq;
        }

        bool operator == (const vector_iterator& b) const {
            return ((*this).m_cur_pos == b.m_cur_pos) &&
                   ((*this).m_vector_ptr == b.m_vector_ptr);
        }

        bool operator != (const vector_iterator& b) const {
            return !((*this)==b);
        }

    };

    // Iterator definition is the nested class
    using const_iterator = vector_iterator;

    const_iterator begin() const {
        return const_iterator(*this, 0);
    }
    
    const_iterator end() const {
        return const_iterator(*this, m_size); 
    }

};
