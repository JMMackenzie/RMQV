#!/bin/bash

# Target index path
TARGET_IDX=/research/remote/petabyte/users/indri-indexes/desires18/ROB04-RED_STOP/

# External collection path
EXT_IDX=/research/remote/petabyte/users/indri-indexes/desires18/BIGNEWS_STOP/

# Sample STOPLIST for docvectors, custom generate from lexicons
# You need to stop any term you would not want to appear in an
# expanded query...
STOPLIST=data/full-stoplist.txt

# 1: Indri to ds2i 
echo "1: Indri --> ds2i..."
mkdir ds2i_raw_target 
../format_index/indri_to_ds2i $TARGET_IDX ds2i_raw_target/robust

mkdir ds2i_raw_external
../format_index/indri_to_ds2i $EXT_IDX ds2i_raw_external/bignews

# 2: Build PEF Indexes
echo "2: Build PEF..."
mkdir target_idx
../build/create_freq_index opt ds2i_raw_target/robust target_idx/robust.opt.pef.idx

mkdir external_idx
../build/create_freq_index opt ds2i_raw_external/bignews external_idx/bignews.opt.pef.idx

echo "3: Build BMW..."
# 3: Build BMW/Wand Data
../build/create_wand_data ds2i_raw_target/robust target_idx/robust-64.bmw BM25

../build/create_wand_data ds2i_raw_external/bignews external_idx/bignews-64.bmw BM25

echo "4: Build document vectors..."
# 4: Build docvectors
../docvector/create_docvectors ds2i_raw_target/robust target_idx/robust.docvector $STOPLIST

../docvector/create_docvectors ds2i_raw_external/bignews external_idx/bignews.docvector $STOPLIST

echo "5: Create param files..."
# 5: Create param files: I've already made them, but you would need
# to do this yourself/write a script/whatever.
# You will need to fix the paths in the param file now.
echo "Building complete. Now, fix the paths in the param files to point to the right places."

echo "Next, we can run some queries."
