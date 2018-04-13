/**
 * Generate queries from probability distribution.
 */

#pragma once

#include <algorithm>
#include <iostream>
#include <cstdint>
#include <limits>
#include <random>
#include <unordered_map>
#include <vector>

/**
 * Generate queries via weighted sampling from a Relevance Model.
 */
class weighted_sampler {
  std::mt19937 prng;

public:
  weighted_sampler(uint64_t s) {
    prng.seed(s);
  }

  std::vector<uint64_t>
  generate_query(std::vector<std::pair<uint32_t, double>>& rm, int min, int max) {
    std::vector<double> cdf(rm.size(), 0.0);

    double c = 0;
    for (size_t i = 0; i < rm.size(); i++) {
      c += rm[i].second;
      cdf[i] = c;
    }

    size_t n = rand(min, max);
    std::vector<uint64_t> qry(n);
    for (size_t i = 0; i < n; i++) {
      qry[i] = rm[bss(cdf, rand())].first;
    }

    return qry;
  }

  size_t bss(std::vector<double>& cdf, double target) {
    size_t high = cdf.size(), low = -1, probe;

    while ((high - low) > 1) {
      probe = (low + high) >> 1;
      if (cdf[probe] < target) {
        low = probe;
      } else {
        high = probe;
      }
    }

    if (high == cdf.size()) {
      high = std::numeric_limits<size_t>::max();
    }

    return high;
  }

  double rand() {
    int max = std::numeric_limits<int>::max();
    std::uniform_real_distribution<> dis(1, max);
    return dis(prng) / double(max);
  }

  int rand(int low, int high) {
    std::uniform_int_distribution<> dis(low, high);
    return dis(prng);
  }
};
