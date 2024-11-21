#! /usr/bin/env python3

import sys
import shutil

if len(sys.argv) == 3:
  shutil.copy(sys.argv[1], sys.argv[2])
else:
  raise Exception("this homemade copy program accepts two arguments")
