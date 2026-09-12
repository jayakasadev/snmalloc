#include <snmalloc/override/rust_config.h>
#include <snmalloc/snmalloc.h>

extern "C" SNMALLOC_SLOW_PATH void* profile_codegen_alloc()
{
  return snmalloc::alloc(32);
}

extern "C" SNMALLOC_SLOW_PATH void profile_codegen_free(void* p)
{
  snmalloc::dealloc(p);
}

int main()
{
  void* p = profile_codegen_alloc();
  profile_codegen_free(p);
  return 0;
}
