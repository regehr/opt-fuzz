set(LLVM_LINK_COMPONENTS
  Core
  IPA
  Support
  )

add_llvm_tool(opt-fuzz
  opt-fuzz.cpp
  )
export_executable_symbols(opt-fuzz)
