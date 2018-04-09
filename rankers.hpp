#pragma once

#include <cmath>
#include <memory>
#include <vector>

namespace ds2i {

// DO NOT permute the ordering of these.
enum ranker_identifier {
    BM25,           // = 1
    LMDS,           // = 2
    UNKNOWN         // Can change
};

// Map a string to a ranker identifier
ranker_identifier get_ranker_id(std::string ranker_name)
{
    if (ranker_name == "BM25") {
        return ranker_identifier::BM25;
    } else if (ranker_name == "LMDS") {
        return ranker_identifier::LMDS;
    } else {
        return ranker_identifier::UNKNOWN;
    }
}

struct doc_scorer {
    double average_doc_len;
    double num_docs;
    double total_terms_in_collection;

    doc_scorer() {}
    doc_scorer(double av_len, double n_doc, double t_term)
        : average_doc_len(av_len)
        , num_docs(n_doc)
        , total_terms_in_collection(t_term)
    {
std::cerr << "total terms is " << total_terms_in_collection << std::endl;
std::cerr << "average doc length is " << average_doc_len << std::endl;
std::cerr << "no. docs is " << num_docs << std::endl;
    }

    // If we need to init after construction
    void init(double av_len, double n_doc, double t_term)
    {
        average_doc_len = av_len;
        num_docs = n_doc;
        total_terms_in_collection = t_term;
    }

    // MUST implement
    virtual double norm_len(const double) const = 0;
    virtual double doc_term_weight(const uint64_t, const double, const uint64_t) const = 0;
    virtual double query_term_weight(const uint64_t, const uint64_t) const = 0;
    virtual std::string name() const = 0;
    virtual ranker_identifier id() const = 0;
    virtual double calculate_document_weight(const uint32_t) const = 0; 
    virtual ~doc_scorer() {} // Avoids memory leaks (I think)
};

struct bm25 : public doc_scorer {

    static constexpr double b = 0.4;
    static constexpr double k1 = 0.9;
    static constexpr double epsilon_score = 1.0E-6;

    bm25() {}
    bm25(double av_len, double n_doc, double t_term)
        : doc_scorer(av_len, n_doc, t_term)
    {
    }

    double norm_len(const double doc_len) const
    {
        return doc_len / average_doc_len;
    }

    double doc_term_weight(const uint64_t f_dt, const double norm_doclen, const uint64_t) const
    {
        double f = (double)f_dt;
        return f / (f + k1 * (1.0f - b + b * norm_doclen));
    }

    double query_term_weight(const uint64_t f_qt, const uint64_t f_t) const
    {
        double f = (double)f_qt;
        double fdf = (double)f_t;
        double idf = std::log((double(num_docs) - fdf + 0.5f) / (fdf + 0.5f));
        return f * std::max(epsilon_score, idf) * (1.0f + k1);
    }

    std::string name() const
    {
        return "BM25";
    }

    ranker_identifier id() const
    {
        return ranker_identifier::BM25;
    }

    double calculate_document_weight(const uint32_t) const
    {
        return 0.0f;
    }


};

constexpr double bm25::epsilon_score;


  struct lmds : public doc_scorer {

    static constexpr double MU = 2500; // Smoothing

    lmds () {}
    lmds(double av_len, double n_doc, double t_term) : doc_scorer(av_len, n_doc, t_term) 
    {
    }

    double norm_len(const double doc_len) const {
        return doc_len;
    }
 
    double doc_term_weight(const uint64_t f_dt, const double norm_len, const uint64_t F_t) const 
    {

        (void) norm_len; // Silence warning
        return std::log( (double(f_dt)/MU) * (total_terms_in_collection/F_t) + 1);
    }

    double query_term_weight(const uint64_t, const uint64_t) const 
    {
        return 1.0f;
    }

    std::string name() const 
    {
        return "LMDS";
    }

    ranker_identifier id() const 
    {
        return ranker_identifier::LMDS;
    }

    // LMDS specific document weight
    double calculate_document_weight(const uint32_t doc_len) const
    {
        return std::log(MU/(MU + double(doc_len)));
    }


  };

// Builds the appropriate ranker based on input params
std::unique_ptr<doc_scorer> build_ranker(double av_doclen, double num_docs,
    double no_terms, ranker_identifier r_id)
{
    std::unique_ptr<doc_scorer> ranker;
    if (r_id == ranker_identifier::BM25) {
        ranker = std::unique_ptr<doc_scorer>(new bm25(av_doclen, num_docs, no_terms));
    } else if (r_id == ranker_identifier::LMDS) {
        ranker = std::unique_ptr<doc_scorer>(new lmds(av_doclen, num_docs, no_terms));
    } else {
        std::cerr << "Cannot instantiate the ranker" << std::endl;
        exit(EXIT_FAILURE);
    }
    return ranker;
}

// Builds the empty ranker based on input params
std::unique_ptr<doc_scorer> build_ranker(ranker_identifier r_id)
{
    std::unique_ptr<doc_scorer> ranker;
    if (r_id == ranker_identifier::BM25) {
        ranker = std::unique_ptr<doc_scorer>(new bm25);
    } else if (r_id == ranker_identifier::LMDS) {
        ranker = std::unique_ptr<doc_scorer>(new lmds);
    } else {
        std::cerr << "Cannot instantiate the ranker" << std::endl;
        exit(EXIT_FAILURE);
    }
    return ranker;
}

} //namespace
