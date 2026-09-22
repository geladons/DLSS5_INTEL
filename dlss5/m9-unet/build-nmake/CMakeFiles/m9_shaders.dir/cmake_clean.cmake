file(REMOVE_RECURSE
  "CMakeFiles/m9_shaders"
  "cosine.spv"
  "cosine_win.spv"
  "elementwise.spv"
  "gather_residual.spv"
  "gemm.spv"
  "gemm_rn1.spv"
  "merge.spv"
  "partition.spv"
  "pool2.spv"
  "softmax.spv"
  "transpose_we.spv"
  "upmerge.spv"
)

# Per-language clean rules from dependency scanning.
foreach(lang )
  include(CMakeFiles/m9_shaders.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
