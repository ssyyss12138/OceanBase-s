#!/usr/bin/env python3
"""Compile the production short-copy functor and check exact byte boundaries.

Requires clang++ (or --cxx) with ASan/UBSan. No database or source rewrite.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cxx', default='clang++')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[3]
source = (repo / 'src/storage/blocksstable/encoding/ob_string_diff_decoder.cpp').read_text()
start = source.index('struct StringDiffCopy\n')
end = source.index('};', start) + 2
production = source[start:end]
preamble = r"""
#include <cstdint>
#include <cstring>
#include <memory>
#include <cassert>
#include <iostream>
#define MEMCPY std::memcpy
"""
checks = r"""
int main() {
  size_t cases = 0;
  // Real source and destination allocations end exactly at the requested
  // byte range: sanitizers catch widened reads/writes beyond either range.
  for (int n = 0; n < 512; ++n) {
    for (int a = 0; a < 16; ++a) for (int b = 0; b < 16; ++b) {
      std::unique_ptr<char[]> src(new char[n+a]);
      std::unique_ptr<char[]> dst(new char[n+b]);
      for (int i = 0; i < n+a; ++i) src[i] = char((i*137+n)%256);
      for (int i = 0; i < n+b; ++i) dst[i] = char(0x5a);
      StringDiffCopy()(src.get()+a, dst.get()+b, n);
      for (int i = 0; i < b; ++i) assert(dst[i] == char(0x5a));
      for (int i = 0; i < n; ++i) assert(dst[i+b] == src[i+a]);
      ++cases;
    }
  }
  StringDiffCopy()(nullptr, nullptr, 0);
  std::cout << "PASS: " << cases
            << " length/alignment/exact-allocation boundary cases\n";
}
"""
with tempfile.TemporaryDirectory(prefix='fts-string-diff-') as directory:
    cpp = Path(directory) / 'contracts.cpp'
    binary = Path(directory) / 'contracts'
    cpp.write_text(preamble + production + checks)
    subprocess.run([args.cxx, '-std=c++17', '-O2', '-fsanitize=address,undefined',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
