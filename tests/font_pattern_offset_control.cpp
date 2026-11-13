// Diagnostic counterexample: a live offset is invisible to conservative LSan.
#include <fontconfig/fontconfig.h>
#include <sanitizer/lsan_interface.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

FcPattern* owner = nullptr;
FcPatternIter iteratorRoot{};
int main() {
 owner=FcPatternCreate();
 if (!owner || !FcPatternAddString(owner,FC_FAMILY,reinterpret_cast<const FcChar8*>("synthetic-offset-test"))) return 2;
 auto a=__lsan_do_recoverable_leak_check();
 std::printf("OWNED RELATIVE-OFFSET PATTERN: lsan=%d\n",a);std::fflush(stdout);
 FcPatternIterStart(owner,&iteratorRoot);
 auto b=__lsan_do_recoverable_leak_check();
 std::printf("SAME PATTERN WITH PUBLIC ITERATOR ROOT: lsan=%d\n",b);std::fflush(stdout);
 FcPatternDestroy(owner);owner=nullptr;std::memset(&iteratorRoot,0,sizeof(iteratorRoot));
 auto c=__lsan_do_recoverable_leak_check();
 std::printf("DESTROYED PATTERN: lsan=%d\n",c);std::fflush(stdout);
 return a==1 && b==0 && c==0 ? 0 : 3;
}
