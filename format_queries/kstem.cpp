/*==========================================================================
 * Copyright (c) 2013 University of Massachusetts.  All Rights Reserved.
 *
 * Use of the Lemur Toolkit for Language Modeling and Information Retrieval
 * is subject to the terms of the software license set forth in the LICENSE
 * file included with this software, and also available at
 * http://www.lemurproject.org/license.html
 *
 *==========================================================================
 */
#include "KrovetzStemmer.hpp"
#include <cstdlib>
#include <cstring>
#include <fstream>

int main(int argc, char *argv[])
{

  char line[8096];
  char word[1024];
  char *rv = NULL;
  stem::KrovetzStemmer *stemmer = new stem::KrovetzStemmer();
  while (fgets(line, 8096, stdin)) {
    char *lptr = line;
    int offset = 0;
    while (sscanf (lptr, "%s%n", word, &offset) == 1) {
      rv = stemmer->kstem_stemmer (word);
      printf ("%s ", rv);
      lptr += offset;
    }
    printf ("\n");
  }
  delete stemmer;
  return (0);
}
