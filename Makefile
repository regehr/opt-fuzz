##===- tools/opt-fuzz/Makefile --------------------------*- Makefile -*-===##
#
#                     The LLVM Compiler Infrastructure
#
# This file is distributed under the University of Illinois Open Source
# License. See LICENSE.TXT for details.
#
##===----------------------------------------------------------------------===##

LEVEL := ../..
TOOLNAME := opt-fuzz
LINK_COMPONENTS := object
LINK_COMPONENTS := bitreader bitwriter asmparser irreader instrumentation scalaropts ipo

include $(LEVEL)/Makefile.common
