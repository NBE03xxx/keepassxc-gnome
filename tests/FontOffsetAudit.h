#pragma once
// Diagnostic only: reconstruct pinned Fontconfig offset and Mesa empty-context
// edges after a raw LSan snapshot. No suppression, no library state changes.
#include <sanitizer/lsan_interface.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fontconfig/fontconfig.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>
#include <string>
#include <sstream>
#include <thread>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>
extern "C" const void* __sanitizer_get_allocated_begin(const void*);
extern "C" size_t __sanitizer_get_allocated_size(const void*);
namespace FontOffsetAudit {
struct Object { uintptr_t address; size_t size; bool direct; std::string origin; bool reached=false; };
struct Edge { uintptr_t from,to; std::string kind; };
struct Pattern { int num,size; intptr_t elements; int references; };
struct Charset { int references,num; intptr_t leaves,numbers; };
inline bool read(int fd,uintptr_t address,void* dst,size_t size) {
 (void)fd;
 iovec local{dst,size},remote{reinterpret_cast<void*>(address),size};
 return ::process_vm_readv(::getpid(),&local,1,&remote,1,0)==ssize_t(size);
}
inline size_t allocated(uintptr_t p) {
 if (!p) return 0;
 const void* v=reinterpret_cast<const void*>(p);
 return __sanitizer_get_allocated_begin(v)==v ? __sanitizer_get_allocated_size(v) : 0;
}
inline int run(const std::string& log) {
 if (FcGetVersion()!=21701 || sizeof(Pattern)!=24 || sizeof(Charset)!=24) {std::fprintf(stderr,"OFFSET AUDIT INCOMPLETE: layout/version\n");return 3;}
 ::unlink((log+".offset-audit.txt").c_str());
 if(std::getenv("KPXC_GUI_OFFSET_NEGATIVE")) {
  std::thread([] {
   auto* leak=std::malloc(37);std::memset(leak,0x5a,37);
   asm volatile("" : : "r"(leak) : "memory");
   auto* pattern=FcPatternCreate();
   FcPatternAddString(pattern,FC_FAMILY,reinterpret_cast<const FcChar8*>("deliberately-lost-font-pattern"));
  }).join();
 }
 const int raw=__lsan_do_recoverable_leak_check();
 std::fflush(stderr);
 {std::ifstream source(log);std::ofstream snapshot(log+".snapshot.log");snapshot<<source.rdbuf();}
 std::ifstream input(log+".snapshot.log");std::string line,origin;bool direct=false,objects=false;std::map<uintptr_t,Object> nodes;
 while(std::getline(input,line)) {
  if(line.rfind("Direct leak of ",0)==0 || line.rfind("Indirect leak of ",0)==0) { direct=line[0]=='D';origin.clear();objects=false; }
  if(line.find("#1 ")!=std::string::npos) origin=line;
  if(line=="Objects leaked above:") { objects=true;continue; }
  unsigned long long address;size_t bytes;
  if(objects && std::sscanf(line.c_str(),"0x%llx (%zu bytes)",&address,&bytes)==2) nodes.emplace(uintptr_t(address),Object{uintptr_t(address),bytes,direct,origin});
  else if(objects && line.empty())objects=false;
 }
 if(raw && nodes.empty()) {std::fprintf(stderr,"OFFSET AUDIT INCOMPLETE: no report objects\n");return 3;}
 int fd=::open("/proc/self/maps",O_RDONLY|O_CLOEXEC);if(fd<0){std::fprintf(stderr,"OFFSET AUDIT INCOMPLETE: map metadata errno=%d\n",errno);return 3;}
 std::vector<Edge> edges;
 auto link=[&](uintptr_t from,uintptr_t to,const char* kind){if(nodes.count(to))edges.push_back({from,to,kind});};
 // Read bounded anonymous writable mappings. Small allocations can occupy
 // non-contiguous mappings, including those first used by a worker thread.
 auto* probe=FcPatternCreate();const auto sample=reinterpret_cast<uintptr_t>(probe);FcPatternDestroy(probe);
 // Mesa 26.0.8's mesa_cache_db is 112 bytes; its mem_ctx is at offset 88.
 // The exact driver binary and source are pinned by the calling harness.
 auto* dbProbe=std::calloc(1,112);const auto dbSample=reinterpret_cast<uintptr_t>(dbProbe);std::free(dbProbe);
 std::map<uintptr_t,uintptr_t> emptyContexts;
 for(const auto& [a,n]:nodes)if(n.size==48 && n.origin.find("libvulkan_radeon.so+0x339949")!=std::string::npos) {
  uintptr_t fields[5]{};
  if(allocated(a)==48 && read(fd,a,fields,sizeof(fields)) && std::all_of(std::begin(fields),std::end(fields),[](auto p){return p==0;}))emptyContexts.emplace(a+48,a);
 }
 std::vector<uintptr_t> smallObjects;
 for(const auto& [a,n]:nodes)if(n.size>=17 && n.size<=32)smallObjects.push_back(a);
 std::ifstream maps("/proc/self/maps");std::vector<std::pair<uintptr_t,uintptr_t>> ranges;
 size_t mappedBytes=0;
 while(std::getline(maps,line)) { unsigned long long a,b;char mode[5]{};int used=0;
  if(std::sscanf(line.c_str(),"%llx-%llx %4s %*s %*s %*s %n",&a,&b,mode,&used)!=3)continue;
  const auto name=line.substr(size_t(used));
  if(mode[0]!='r'||mode[1]!='w'||mode[3]!='p'||b-a>64*1024*1024)continue;
  if(!name.empty() && name!="[heap]" && name.rfind("[anon:",0)!=0)continue;
  auto object=std::lower_bound(smallObjects.begin(),smallObjects.end(),uintptr_t(a));
  if(!(a<=sample && sample<b) && !(a<=dbSample && dbSample<b) && !(object!=smallObjects.end() && *object<b))continue;
  mappedBytes+=b-a;if(mappedBytes>512*1024*1024){std::fprintf(stderr,"OFFSET AUDIT INCOMPLETE: scan budget\n");::close(fd);return 3;}
  ranges.push_back({uintptr_t(a),uintptr_t(b)});
 }
 size_t owners=0,mesaOwners=0,dbCandidates=0,contextMatches=0;std::vector<char> buffer(65536+112);
 for(auto [start,end]:ranges)for(uintptr_t base=start;base<end;base+=65536) {
  size_t length=std::min<size_t>(buffer.size(),end-base);
  if(!read(fd,base,buffer.data(),length))continue;
  for(size_t off=0;off<65536 && off+24<=length;off+=16) {
   uintptr_t address=base+off;Pattern p;std::memcpy(&p,buffer.data()+off,24);
   if(!emptyContexts.empty() && off+112<=length) {
    uintptr_t context=0;std::memcpy(&context,buffer.data()+off+88,8);
    if(allocated(address)==112)++dbCandidates;
    if(emptyContexts.count(context))++contextMatches;
    if(auto found=emptyContexts.find(context);found!=emptyContexts.end() && allocated(address)==112 && !nodes.count(address)) {
     // A live, unreported DB owner holds exactly the one-past-end pointer
     // returned by ralloc_context(NULL). Never root a reported/lost DB.
     uintptr_t index=0,cacheFile=0,indexFile=0;
     std::memcpy(&index,buffer.data()+off,8);std::memcpy(&cacheFile,buffer.data()+off+8,8);std::memcpy(&indexFile,buffer.data()+off+40,8);
     // index_db itself is a nonempty ralloc allocation (an interior pointer).
     // mesa_db_unlock closes both FILEs without releasing the cache DB.
     // Check the persistent paths/UUIDs; allow closed FILEs or valid FILE magic.
     uint32_t cacheFlags=0,indexFlags=0;
     read(fd,cacheFile,&cacheFlags,4);read(fd,indexFile,&indexFlags,4);
     uintptr_t cachePath=0,indexPath=0;uint64_t cacheUuid=0,indexUuid=0,uuid=0;
     std::memcpy(&cachePath,buffer.data()+off+16,8);std::memcpy(&indexPath,buffer.data()+off+48,8);
     std::memcpy(&cacheUuid,buffer.data()+off+32,8);std::memcpy(&indexUuid,buffer.data()+off+64,8);std::memcpy(&uuid,buffer.data()+off+96,8);
     // Newly created files have zero cached UUID fields until header reload;
     // mesa_db_recreate_files writes the generated UUID directly to disk.
     const bool validUuid=uuid && cacheUuid==indexUuid && (!cacheUuid || cacheUuid==uuid);
     if(__sanitizer_get_allocated_begin(reinterpret_cast<void*>(index)) && allocated(cachePath)>1 && allocated(indexPath)>1 && validUuid && buffer[off+104]==1 && (!cacheFile || (cacheFlags&0xffff0000u)==0xfbad0000u) && (!indexFile || (indexFlags&0xffff0000u)==0xfbad0000u)) {link(address,found->second,"mesa-empty-context");++mesaOwners;}
    }
   }
   bool maybePattern=p.num>0 && p.num<=p.size && p.size>0 && p.size<65536 && p.size%16==0 && p.references>0 && p.references<1000000;
   Charset c;std::memcpy(&c,buffer.data()+off,24);
   bool maybeCharset=c.references>0 && c.references<1000000 && c.num>0 && c.num<=4352;
   if(!(maybePattern||maybeCharset) || allocated(address)!=24)continue;
   if(maybePattern) {
    uintptr_t elements=address+uintptr_t(p.elements);
    if(nodes.count(elements) && nodes.at(elements).size==size_t(p.size)*16 && allocated(elements)==size_t(p.size)*16) { link(address,elements,"pattern-elements");++owners; }
   }
   if(maybeCharset) {
    uintptr_t leaves=address+uintptr_t(c.leaves),numbers=address+uintptr_t(c.numbers);
    size_t leafBytes=allocated(leaves),numberBytes=allocated(numbers);
    if(leafBytes<size_t(c.num)*8 || numberBytes<size_t(c.num)*2 || leafBytes>65536 || numberBytes>16384 || leafBytes!=numberBytes*4)continue;
    std::vector<intptr_t> offsets(c.num);
    if(!read(fd,leaves,offsets.data(),offsets.size()*8))continue;
    bool valid=true;
    for(auto offset:offsets)if(allocated(leaves+uintptr_t(offset))!=32){valid=false;break;}
    if(!valid)continue;
    link(address,leaves,"charset-leaves");link(address,numbers,"charset-numbers");
    for(auto offset:offsets)link(leaves,leaves+uintptr_t(offset),"charset-leaf");
    ++owners;
   }
  }
 }
 // Preserve LSan's ordinary pointer edges between reported allocations.
 // Only relative offsets add new edges; none add a new ownership reference.
 auto target=[&](uintptr_t value)->uintptr_t {
  auto it=nodes.upper_bound(value);if(it==nodes.begin())return 0;--it;
  return value-it->first<it->second.size ? it->first : 0;
 };
 for(auto& [address,node]:nodes) {
  if(allocated(address)!=node.size)continue;
  std::vector<uintptr_t> words(node.size/sizeof(uintptr_t));
  if(!read(fd,address,words.data(),words.size()*sizeof(uintptr_t)))continue;
  for(auto word:words)if(auto to=target(word))edges.push_back({address,to,"native-pointer"});
 }
 ::close(fd);
 bool changed=true;size_t rounds=0;
 while(changed && ++rounds<=nodes.size()+1) {
  changed=false;
  for(const auto& e:edges)if(!nodes.count(e.from) || nodes.at(e.from).reached) {
   if(!nodes.at(e.to).reached){nodes.at(e.to).reached=true;changed=true;}
  }
 }
 size_t explained=0,unexplained=0,directUnknown=0;
 for(const auto& [a,n]:nodes) {
  if(n.reached)explained+=n.size;
  else {unexplained+=n.size;if(n.direct)++directUnknown;}
 }
 std::ofstream report(log+".offset-audit.txt");
 std::ofstream ownership(log+".ownership-edges.tsv");
 ownership<<"source\tsource_bytes\tsource_reported\ttarget\tkind\n";
 for(const auto& edge:edges)if(edge.kind!="native-pointer")ownership<<std::hex<<edge.from<<std::dec<<'\t'<<allocated(edge.from)<<'\t'<<nodes.count(edge.from)<<'\t'<<std::hex<<edge.to<<std::dec<<'\t'<<edge.kind<<'\n';
 report<<"fontconfig_version="<<FcGetVersion()<<" sizeof_pattern="<<sizeof(Pattern)<<" sizeof_charset="<<sizeof(Charset)<<" scan_bytes="<<mappedBytes<<" empty_contexts="<<emptyContexts.size()<<" db_candidates="<<dbCandidates<<" context_matches="<<contextMatches<<"\n";
 report<<"raw_lsan="<<raw<<" objects="<<nodes.size()<<" live_offset_owners="<<owners<<" live_mesa_owners="<<mesaOwners<<" edges="<<edges.size()<<" explained_bytes="<<explained<<" unexplained_bytes="<<unexplained<<" unexplained_direct_objects="<<directUnknown<<"\n";
 for(const auto& [a,n]:nodes)if(!n.reached)report<<"unexplained "<<std::hex<<a<<std::dec<<" size="<<n.size<<" direct="<<n.direct<<" "<<n.origin<<"\n";
 std::printf("OFFSET AUDIT: raw=%d objects=%zu owners=%zu explained=%zu unexplained=%zu\n",raw,nodes.size(),owners,explained,unexplained);std::fflush(stdout);
 return unexplained ? 1 : 0;
}
}
