#!/bin/bash

# 1: Single-shot, traditional query expansion
../build/single_shot_expansion opt block_max_wand param_files/robust.param --output robust-qe.run --query data/queries.txt

# 2: External corpus expansion (RM3 from ext)
../build/external_corpus_expansion opt block_max_wand param_files/robust.param --external param_files/bignews.param --output external-qe.run --query data/queries.txt

# 3: External corpus BOW sampling
../build/external_corpus_sampler opt block_max_wand param_files/robust.param --external param_files/bignews.param --output external-sampler.run --query data/queries.txt


