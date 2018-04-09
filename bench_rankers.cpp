#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <thread>

#include "rankers.hpp"

const size_t MAX_QRY_TERMS = 20;
const size_t NUM_EVALS = 10000000;
const size_t DOCS_IN_COL = 25000000;
const float AVG_DOC_LEN = 150.0f;
const size_t NUM_TERMS = 35000000;
const size_t NUM_BENCH_RUNS = 5;

struct term_data {
    float f_qt;
    float f_dt;
    float f_T;
};

struct doc_data {
    float doc_len;
    uint64_t query_terms;
    term_data terms[MAX_QRY_TERMS];
};

std::vector<doc_data> generate_dummy_data(size_t qterms)
{
    std::vector<doc_data> dummy_data(NUM_EVALS);
    std::mt19937 gen{ 12345 };
    std::normal_distribution<> doc_len_dist{ 150, 15 };
    std::normal_distribution<> fqt_dist{ 1, 2 };
    std::normal_distribution<> fdt_dist{ 1, 3 };
    std::normal_distribution<> fT_dist{ 10000000, 50000 };
    for (size_t i = 0; i < NUM_EVALS; i++) {
        auto& d = dummy_data[i];
        d.doc_len = std::round(doc_len_dist(gen));
        d.query_terms = qterms;
        for (size_t j = 0; j < qterms; j++) {
            d.terms[j].f_qt = std::max(1.0, std::round(fqt_dist(gen)));
            d.terms[j].f_dt = std::max(1.0, std::round(fdt_dist(gen)));
            d.terms[j].f_T = std::max(1.0, std::round(fT_dist(gen)));
        }
    }
    return dummy_data;
}

void benchmark_ranker(std::map<std::string, float>& timings, std::unique_ptr<ds2i::doc_scorer>& ptr, const std::vector<doc_data>& data)
{
    float F_T = NUM_TERMS;
    auto& ranker = *ptr.get();
    std::vector<std::chrono::nanoseconds> run_times;
    for (size_t j = 0; j < NUM_BENCH_RUNS; j++) {
        float score = 0.0f;
        auto start = std::chrono::high_resolution_clock::now();
        for (size_t i = 0; i < NUM_EVALS; i++) {
            const auto& cur_doc = data[i];
            float norm_doc_len = ranker.norm_len(cur_doc.doc_len);
            float doc_score = 0.0f;
            for (size_t k = 0; k < cur_doc.query_terms; k++) {
                float q_weight = ranker.query_term_weight(cur_doc.terms[k].f_qt, cur_doc.terms[k].f_T);
                float doc_weight = ranker.doc_term_weight(cur_doc.terms[k].f_dt, norm_doc_len, F_T);
                doc_score += q_weight * doc_weight;
            }
            score += doc_score;
        }
        auto end = std::chrono::high_resolution_clock::now();
        run_times.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start));
        if (score == 0) {
            std::cout << "make sure score is never optimised away..." << std::endl;
        }
    }
    std::sort(run_times.begin(), run_times.end());
    timings[ranker.name()] = double(run_times[run_times.size() / 2].count()) / double(NUM_EVALS);
}

int main()
{
    using namespace ds2i;
    for (size_t qterms = 1; qterms <= MAX_QRY_TERMS; qterms++) {
        auto dummy_doc_data = generate_dummy_data(qterms);
        std::map<std::string, float> timings;
        for (auto ranker_id : all_rankers) {
            auto ranker = build_ranker(AVG_DOC_LEN, DOCS_IN_COL, NUM_TERMS, ranker_id);
            benchmark_ranker(timings, ranker, dummy_doc_data);
        }
        if (qterms == 1) {
            // header
            std::cout << "qterms,";
            for (auto& t : timings) {
                std::cout << t.first << ",";
            }
            std::cout << std::endl;
        }
        std::cout << qterms << ",";
        for (auto& t : timings) {
            std::cout << t.second << ",";
        }
        std::cout << std::endl;
    }
    return 0;
}
