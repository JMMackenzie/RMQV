/**
 * RRF fusion.
 */

#pragma once

#include <algorithm>
#include <iostream>
#include <cstdint>
#include <unordered_map>
#include <vector>

/**
 * RRF fuse a top-k result list of <double, uint64_t> pairs.
 */
class document_fuser {
  static constexpr int k = 60;

  public:
  /**
   * Fuse `n` result lists at the same time.
   */
  static void hot_fuse(
      std::vector<std::vector<std::pair<double, uint64_t>>>& res_lists,
      std::vector<std::pair<double, uint64_t>>& dest) {
    std::unordered_map<uint64_t, double> accum;

    for (size_t i = 0; i < res_lists.size(); i++) {
      for (size_t j = 0; j < res_lists[i].size(); j++) {
        auto key = res_lists[i][j].second;
        accum[key] += 1.0 / (k + j + 1);
      }
    }

    // assume `dest` is empty
    for (const auto& d : accum) {
      dest.push_back({d.second, d.first});
    }
    std::sort(dest.begin(), dest.end(),
              [](const std::pair<double, uint64_t>& a,
                 const std::pair<double, uint64_t>& b) {
              return a.first > b.first;
    });
  }
};
