#!/bin/bash

set -eu

# Target Indri index path
TARGET_IDX=/path/to/indri/target_index/

# External Indri collection path
EXT_IDX=/path/to/indri/external_index/

# Sample STOPLIST for docvectors, custom generate from lexicons
# You need to stop any term you would not want to appear in an
# expanded query...
STOPLIST=data/full-stoplist.txt

# 1: Indri to ds2i 
echo "1: Indri --> ds2i..."
mkdir -p ds2i_raw_target
../build/indri_to_ds2i $TARGET_IDX ds2i_raw_target/target

mkdir -p ds2i_raw_external
../build/indri_to_ds2i $EXT_IDX ds2i_raw_external/external

# 2: Build PEF Indexes
echo "2: Build PEF..."
mkdir -p target_idx
../build/create_freq_index opt ds2i_raw_target/target target_idx/target.opt.pef.idx

mkdir -p external_idx
../build/create_freq_index opt ds2i_raw_external/external external_idx/external.opt.pef.idx

echo "3: Build BMW..."
# 3: Build BMW/Wand Data
../build/create_wand_data ds2i_raw_target/target target_idx/target-64.bmw BM25

../build/create_wand_data ds2i_raw_external/external external_idx/external-64.bmw BM25

echo "4: Build document vectors..."
# 4: Build docvectors
../build/create_docvectors ds2i_raw_target/target target_idx/target.docvector $STOPLIST

../build/create_docvectors ds2i_raw_external/external external_idx/external.docvector $STOPLIST

echo "5: Create param files..."
# 5: Create param files: I've already made them, but you would need
# to do this yourself/write a script/whatever.
# You will need to fix the paths in the param file now.
echo "Building complete. Now, fix the paths in the param files to point to the right places."

echo "Next, we can run some queries."
