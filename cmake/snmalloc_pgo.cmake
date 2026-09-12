# snmalloc PGO support
# ---------------------------------------------------------------------------
#
# Two-stage Profile-Guided Optimization for snmalloc. Driven by the cache
# variable SNMALLOC_PROFILE_PGO which takes one of:
#   off       - default; no PGO flags added.
#   generate  - emit a profile-generate build. Run the resulting binaries
#               against a representative workload; .profraw / .gcda files
#               will be written to SNMALLOC_PGO_PROFILE_DIR (clang) or to
#               the binary's runtime working dir (gcc).
#   use       - consume a previously-merged profile from
#               SNMALLOC_PGO_PROFILE_FILE (clang/llvm-profdata format) or
#               SNMALLOC_PGO_PROFILE_DIR (gcc .gcda tree) to produce the
#               final optimized library + bench binaries.
#
# Flags go through add_compile_options / add_link_options so they reach
# every target. PGO needs that: instrumentation must be in every .o, and
# the matching libgcov / libclang_rt.profile must be on the link line.
#
# Only Clang/AppleClang and GCC are supported; MSVC PGO fails with an
# error rather than silently producing a non-PGO build.
#
# Use the same compiler for both stages: the LLVM raw profile format
# changes between major clang releases.

if (DEFINED _SNMALLOC_PGO_INCLUDED)
  return()
endif()
set(_SNMALLOC_PGO_INCLUDED TRUE)

set(SNMALLOC_PROFILE_PGO "off" CACHE STRING
  "PGO stage: off, generate, or use")
set_property(CACHE SNMALLOC_PROFILE_PGO PROPERTY STRINGS off generate use)

set(SNMALLOC_PGO_PROFILE_DIR "${CMAKE_BINARY_DIR}/pgo-data" CACHE PATH
  "Directory to write PGO .profraw / .gcda files during a generate build, \
or to read .gcda from during a gcc use build.")

set(SNMALLOC_PGO_PROFILE_FILE "" CACHE FILEPATH
  "Merged .profdata file to consume during a clang use build. Produced by \
`llvm-profdata merge -o <file> <SNMALLOC_PGO_PROFILE_DIR>/*.profraw`.")

# Normalize to lowercase and validate.
string(TOLOWER "${SNMALLOC_PROFILE_PGO}" _snmalloc_pgo_stage)
set(_snmalloc_pgo_valid off generate use)
if (NOT _snmalloc_pgo_stage IN_LIST _snmalloc_pgo_valid)
  message(FATAL_ERROR
    "SNMALLOC_PROFILE_PGO=${SNMALLOC_PROFILE_PGO} is not one of: \
off, generate, use")
endif()

if (_snmalloc_pgo_stage STREQUAL "off")
  return()
endif()

set(_snmalloc_pgo_compiler_id "${CMAKE_CXX_COMPILER_ID}")
set(_snmalloc_pgo_is_clang FALSE)
set(_snmalloc_pgo_is_gcc FALSE)
if (_snmalloc_pgo_compiler_id STREQUAL "Clang" OR
    _snmalloc_pgo_compiler_id STREQUAL "AppleClang")
  set(_snmalloc_pgo_is_clang TRUE)
elseif (_snmalloc_pgo_compiler_id STREQUAL "GNU")
  set(_snmalloc_pgo_is_gcc TRUE)
else()
  message(FATAL_ERROR
    "SNMALLOC_PROFILE_PGO=${SNMALLOC_PROFILE_PGO} requires Clang/AppleClang \
or GCC (got ${_snmalloc_pgo_compiler_id}). MSVC PGO is not wired up.")
endif()

# Ensure the data dir exists for the generate stage. For the use stage
# we don't create it: missing input should fail loudly later.
if (_snmalloc_pgo_stage STREQUAL "generate")
  file(MAKE_DIRECTORY "${SNMALLOC_PGO_PROFILE_DIR}")
endif()

if (_snmalloc_pgo_is_clang)
  if (_snmalloc_pgo_stage STREQUAL "generate")
    # -fprofile-generate=<dir> writes default_%m_%p.profraw under <dir>.
    # We pass the absolute path so the data lands in the build tree
    # regardless of where the trained binary is launched from.
    set(_snmalloc_pgo_flag "-fprofile-generate=${SNMALLOC_PGO_PROFILE_DIR}")
    add_compile_options(${_snmalloc_pgo_flag})
    add_link_options(${_snmalloc_pgo_flag})
    message(STATUS
      "snmalloc PGO: clang generate stage, profile data -> \
${SNMALLOC_PGO_PROFILE_DIR}")
  elseif (_snmalloc_pgo_stage STREQUAL "use")
    if (SNMALLOC_PGO_PROFILE_FILE STREQUAL "")
      message(FATAL_ERROR
        "SNMALLOC_PROFILE_PGO=use requires SNMALLOC_PGO_PROFILE_FILE to \
point at a merged .profdata file.")
    endif()
    if (NOT EXISTS "${SNMALLOC_PGO_PROFILE_FILE}")
      message(FATAL_ERROR
        "SNMALLOC_PGO_PROFILE_FILE=${SNMALLOC_PGO_PROFILE_FILE} does not \
exist. Run llvm-profdata merge first.")
    endif()
    set(_snmalloc_pgo_flag "-fprofile-use=${SNMALLOC_PGO_PROFILE_FILE}")
    add_compile_options(${_snmalloc_pgo_flag})
    add_link_options(${_snmalloc_pgo_flag})
    # Silence hash-mismatch warnings between the two stages: they are
    # routine when code changes in between. Functions whose hashes still
    # match are optimized as normal.
    add_compile_options(-Wno-profile-instr-out-of-date
                        -Wno-profile-instr-unprofiled
                        -Wno-backend-plugin)
    message(STATUS
      "snmalloc PGO: clang use stage, consuming \
${SNMALLOC_PGO_PROFILE_FILE}")
  endif()
elseif (_snmalloc_pgo_is_gcc)
  # gcc writes .gcda next to the .gcno under the original build path.
  # -fprofile-dir lets us redirect that to the user-visible data dir so
  # both stages share a stable location.
  if (_snmalloc_pgo_stage STREQUAL "generate")
    add_compile_options(-fprofile-generate
                        "-fprofile-dir=${SNMALLOC_PGO_PROFILE_DIR}")
    add_link_options(-fprofile-generate
                     "-fprofile-dir=${SNMALLOC_PGO_PROFILE_DIR}")
    message(STATUS
      "snmalloc PGO: gcc generate stage, profile data -> \
${SNMALLOC_PGO_PROFILE_DIR}")
  elseif (_snmalloc_pgo_stage STREQUAL "use")
    if (NOT EXISTS "${SNMALLOC_PGO_PROFILE_DIR}")
      message(FATAL_ERROR
        "SNMALLOC_PGO_PROFILE_DIR=${SNMALLOC_PGO_PROFILE_DIR} does not \
exist. Run the generate stage and execute the training workload first.")
    endif()
    add_compile_options(-fprofile-use
                        "-fprofile-dir=${SNMALLOC_PGO_PROFILE_DIR}"
                        -fprofile-correction
                        -Wno-coverage-mismatch
                        -Wno-missing-profile)
    add_link_options(-fprofile-use
                     "-fprofile-dir=${SNMALLOC_PGO_PROFILE_DIR}")
    message(STATUS
      "snmalloc PGO: gcc use stage, consuming \
${SNMALLOC_PGO_PROFILE_DIR}")
  endif()
endif()

# Record the PGO stage on the snmalloc interface target so downstream
# code (such as snmalloc-rs build.rs) can detect it. Guarded so this file
# works whether it is included before or after the snmalloc target.
function(_snmalloc_pgo_tag_target)
  if (TARGET snmalloc)
    target_compile_definitions(snmalloc INTERFACE
      SNMALLOC_PGO_STAGE="${_snmalloc_pgo_stage}")
  endif()
endfunction()
cmake_language(DEFER CALL _snmalloc_pgo_tag_target)
