#!/usr/bin/env python3
"""Build a disposable copy of the frozen GUI probe for allocation/animation contrasts.
Source tools/dev-env.sh first. No product binary or installed setting is changed.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('mode',choices=('profile','no-animation','dialog-count'))
parser.add_argument('--seconds',type=int)
parser.add_argument('--idle',type=int,default=15)
parser.add_argument('--tag',default='')
args=parser.parse_args()
seconds=args.seconds if args.seconds is not None else (900 if args.mode=='profile' else 300)
assert 60<=seconds<=1800,'bounded diagnostic duration'
assert 15<=args.idle<=900,'bounded idle duration'
import re
assert not args.tag or re.fullmatch(r'[a-z][a-z0-9-]{0,5}',args.tag),'short safe output tag'
project=Path(__file__).resolve().parents[1]
os.chdir(project)
root=project/'build/gc'/f'{args.tag+"-" if args.tag else ""}{args.mode[0]}{seconds}'
root.mkdir(parents=True,exist_ok=False)
pin=json.loads((project/'tests/font-offset-audit.json').read_text())
for library,key in [('libfontconfig.so.1','fontconfig_sha256'),('libvulkan_radeon.so','mesa_sha256')]:
    path=project/'.cache/development/sysroot/usr/lib/x86_64-linux-gnu'/library
    assert hashlib.sha256(path.read_bytes()).hexdigest()==pin[key],'dependency changed: review diagnostic'
original=(project/'tests/lifetime_ui_probe.cpp').read_text()
assert original.count('int main(int argc, char** argv) {')==1,'probe structure changed'
clone=original.replace('int main(int argc, char** argv) {','int original_lifetime_main(int argc, char** argv) {')
clone=clone.replace('"../src/main.cpp"',f'"{project}/src/main.cpp"')
for header in ('SoakMetrics.h','FontOffsetAudit.h'):
    clone=clone.replace(f'"{header}"',f'"{project}/tests/{header}"')
if args.mode=='dialog-count':
    clone='''#include <cstdio>
static unsigned controlCreated=0,controlFinalized=0;
static void dialogCounts(const char* phase) {
    std::fprintf(stderr,"DIALOG LIFETIME phase=%s created=%u finalized=%u retained=%u\\n",phase,controlCreated,controlFinalized,controlCreated-controlFinalized);
}
'''+clone
    getter='AdwDialog* dialog() { return adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(ui->window)); }'
    assert clone.count(getter)==1,'dialog getter changed'
    clone=clone.replace(getter,'''AdwDialog* dialog() {
        auto* d=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(ui->window));
        if(d && !g_object_get_data(G_OBJECT(d),"control-finalize-count")) {
            ++controlCreated;
            g_object_set_data_full(G_OBJECT(d),"control-finalize-count",&controlFinalized,
                +[](gpointer data){++*static_cast<unsigned*>(data);});
        }
        return d;
    }''')
    hook='cacheStats(); metrics->sample(phase, completedCycles, force);'
    assert clone.count(hook)==1
    clone=clone.replace(hook,hook+' if(force)dialogCounts(phase);')
    final='metrics.sample("finalized", completed);'
    assert clone.count(final)==1
    clone=clone.replace(final,final+' dialogCounts("finalized");')
if args.mode=='profile':
    # Sample the same completed locked phase, not arbitrary in-flight forms.
    # No cache frequency/timestamp or production behavior is changed.
    clone='#include <cstddef>\nextern "C" void __sanitizer_print_memory_profile(size_t,size_t);\n'+clone
    sample='cacheStats(); metrics->sample(phase, completedCycles, force);'
    assert clone.count(sample)==1,'phase sample hook changed'
    clone=clone.replace(sample,sample+'''
        static unsigned next=60;
        const auto elapsed=(g_get_monotonic_time()-metrics->start)/G_USEC_PER_SEC;
        if ((std::strcmp(phase,"cycle-locked")==0 && elapsed>=next) || std::strcmp(phase,"idle-end")==0) {
            std::fprintf(stderr,"ALLOCATION PROFILE elapsed_s=%ld phase=%s cycle=%u clears=%u\\n",
                         long(elapsed),phase,completedCycles,metrics->fontCacheClears);std::fflush(stderr);
            __sanitizer_print_memory_profile(100,200);
            next=(unsigned(elapsed)/300+1)*300;
        }
''')
    final='metrics.sample("finalized", completed);'
    assert clone.count(final)==1,'finalization sample hook changed'
    clone=clone.replace(final,final+'''
        std::fprintf(stderr,"ALLOCATION PROFILE elapsed_s=%ld phase=finalized cycle=%u clears=%u\\n",
                     long((g_get_monotonic_time()-metrics.start)/G_USEC_PER_SEC),completed,metrics.fontCacheClears);
        std::fflush(stderr);__sanitizer_print_memory_profile(100,200);
''')
(root/'lifetime.cpp').write_text(clone)
footer='''
#include "lifetime.cpp"
'''
if args.mode=='profile':
    footer+='''
int main(int argc,char** argv) {
    return original_lifetime_main(argc,argv);
}
'''
elif args.mode=='no-animation':
    footer+='''
int main(int argc,char** argv) {
    initializeUiLocale();gtk_init();
    g_object_set(gtk_settings_get_default(),"gtk-enable-animations",FALSE,nullptr);
    std::puts("CONTROL: animations disabled in this disposable process only");std::fflush(stdout);
    return original_lifetime_main(argc,argv);
}
'''
else:
    footer+='int main(int argc,char** argv) { return original_lifetime_main(argc,argv); }\n'
source=root/'control.cpp';source.write_text(footer)
commands=subprocess.check_output(['ninja','-C','build/reader-asan','-t','commands','gnome_lifetime_ui_probe'],text=True).splitlines()
compile=shlex.split(next(s for s in commands if '/tests/lifetime_ui_probe.cpp' in s and ' -c ' in s))
for i,value in enumerate(compile):
    if value.endswith('/tests/lifetime_ui_probe.cpp'):compile[i]=str(source)
    if i and compile[i-1] in ('-o','-MT'):compile[i]=str(root/'control.o')
    if i and compile[i-1]=='-MF':compile[i]=str(root/'control.d')
subprocess.run(compile,cwd=project/'build/reader-asan',check=True)
link=shlex.split(commands[-1]);link=link[link.index('&&')+1:]
if '&&' in link:link=link[:link.index('&&')]
for i,value in enumerate(link):
    if value.endswith('lifetime_ui_probe.cpp.o'):link[i]=str(root/'control.o')
    if i and link[i-1]=='-o':link[i]=str(root/'control')
subprocess.run(link,cwd=project/'build/reader-asan',check=True)
harness=(project/'tools/test-ui.sh').read_text()
cd='cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."'
copy='cp build/reader-asan-output/bin/gnome_lifetime_ui_probe '
assert harness.count(cd)==1 and harness.count(copy)==1,'private harness changed'
harness=harness.replace(cd,'cd -- '+shlex.quote(str(project))).replace(copy,'cp '+shlex.quote(str(root/'control'))+' ')
(root/'ui.sh').write_text(harness)
paths=[source,root/'lifetime.cpp',root/'control',root/'ui.sh',project/'src/main.cpp',project/'src/gnome/FontCache.h',project/'tests/SoakMetrics.h',project/'tests/FontOffsetAudit.h',project/'tools/gui-memory-control.py',project/'build/bin/keepassxc-gnome-worker']
config={'test_mode':args.mode+' control','requested_active_s':seconds,'requested_idle_s':args.idle,
        'renderer':'vulkan','mesa_shader_cache_disable':os.environ.get('MESA_SHADER_CACHE_DISABLE','unset'),
        'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'source_binary_sha256':{str(p.relative_to(project)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},'dependency_pin':pin}
(root/'run-config.json').write_text(json.dumps(config,indent=2)+'\n')
env=os.environ.copy()
for key in ('LD_PRELOAD','KPXC_GUI_OFFSET_NEGATIVE','KPXC_VULKAN_HOLD_DRIVER'):env.pop(key,None)
env.update(KPXC_GUI_AUDIT_DIR=str(root),KPXC_GUI_OFFSET_AUDIT='1',KPXC_GUI_DETECT_LEAKS='1',KPXC_GUI_CYCLES='1',
           KPXC_GUI_SOAK_SECONDS=str(seconds),KPXC_GUI_IDLE_SECONDS=str(args.idle),KPXC_UI_TIMEOUT=str(seconds+args.idle+180),
           KPXC_UI_PASS='OFFSET AUDIT:',KPXC_UI_EXIT_ATTEMPTS='1000',GSK_RENDERER='vulkan',LSAN_OPTIONS='report_objects=1')
with (root/'harness.log').open('w') as output:
    result=subprocess.run(['bash',str(root/'ui.sh'),'--lifetime-asan'],env=env,stdout=output,stderr=subprocess.STDOUT)
(root/'raw-exit-status').write_text(str(result.returncode)+'\n')
raise SystemExit(subprocess.run(['python3',str(project/'tools/check-font-offset-audit.py'),str(root)]).returncode)
