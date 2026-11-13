// Diagnostic counterexample, pinned to Fontconfig 2.17.1 LP64.
#include <fontconfig/fontconfig.h>
#include <sanitizer/lsan_interface.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
// Diagnostic representation checked against this exact Fontconfig source,
// not a product ABI dependency. memcpy avoids aliasing through an opaque type.
struct CharSetLayout { int refs; int count; std::intptr_t leaves, numbers; };
FcCharSet* owner = nullptr;
void* decodedRoots[20]{};
int main() {
 if (FcGetVersion()!=21701 || sizeof(CharSetLayout)!=24) return 2;
 owner=FcCharSetCreate();FcCharSetAddChar(owner,'A');FcCharSetAddChar(owner,0x3042);
 auto a=__lsan_do_recoverable_leak_check();
 std::printf("OWNED CHARSET OFFSET GRAPH: lsan=%d\n",a);std::fflush(stdout);
 CharSetLayout layout;std::memcpy(&layout,owner,sizeof layout);
 if (layout.count!=2 || !FcCharSetHasChar(owner,'A') || !FcCharSetHasChar(owner,0x3042)) return 2;
 auto* leaves=reinterpret_cast<std::intptr_t*>(reinterpret_cast<std::uintptr_t>(owner)+layout.leaves);
 decodedRoots[0]=leaves;decodedRoots[1]=reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(owner)+layout.numbers);
 for (int i=0;i<layout.count;++i) decodedRoots[2+i]=reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(leaves)+leaves[i]);
 auto b=__lsan_do_recoverable_leak_check();
 std::printf("SAME CHARSET WITH DECODED GRAPH: lsan=%d\n",b);std::fflush(stdout);
 FcCharSetDestroy(owner);owner=nullptr;std::memset(decodedRoots,0,sizeof decodedRoots);std::memset(&layout,0,sizeof layout);
 auto c=__lsan_do_recoverable_leak_check();
 std::printf("DESTROYED CHARSET: lsan=%d\n",c);std::fflush(stdout);
 return a==1 && b==0 && c==0 ? 0 : 3;
}
