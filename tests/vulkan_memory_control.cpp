// Standalone synthetic driver control. No GTK, Qt, crypto or project model.
#include <vulkan/vulkan.h>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string>
#include <dlfcn.h>
static void traceMaps(const char* phase) {
    if (const auto* root=std::getenv("KPXC_VULKAN_TRACE_ROOT")) {
        std::ifstream input("/proc/self/maps");
        std::ofstream output(std::string(root)+"/maps-"+phase+".txt");
        output<<input.rdbuf();
    }
}
static int run(bool device) {
    VkInstanceCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    VkInstance instance{};auto result=vkCreateInstance(&info,nullptr,&instance);
    if(result!=VK_SUCCESS){std::fprintf(stderr,"Control Vulkan instance failed: %d\n",result);return 2;}
    uint32_t count=0;result=vkEnumeratePhysicalDevices(instance,&count,nullptr);
    if(result!=VK_SUCCESS || !count){vkDestroyInstance(instance,nullptr);return 2;}
    std::vector<VkPhysicalDevice> devices(count);result=vkEnumeratePhysicalDevices(instance,&count,devices.data());
    if(result!=VK_SUCCESS){vkDestroyInstance(instance,nullptr);return 2;}
    if(device)for(auto physical:devices) {
        uint32_t queues=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&queues,nullptr);
        std::vector<VkQueueFamilyProperties> families(queues);vkGetPhysicalDeviceQueueFamilyProperties(physical,&queues,families.data());
        uint32_t index=0;while(index<queues && !families[index].queueCount)++index;
        if(index==queues){vkDestroyInstance(instance,nullptr);return 2;}
        const float priority=1.f;VkDeviceQueueCreateInfo queue{};queue.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex=index;queue.queueCount=1;queue.pQueuePriorities=&priority;
        VkDeviceCreateInfo create{};create.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;create.queueCreateInfoCount=1;create.pQueueCreateInfos=&queue;
        VkDevice logical{};result=vkCreateDevice(physical,&create,nullptr,&logical);
        if(result!=VK_SUCCESS){vkDestroyInstance(instance,nullptr);return 2;}
        vkDestroyDevice(logical,nullptr);
    }
    traceMaps("loaded");
    vkDestroyInstance(instance,nullptr);traceMaps("destroyed");
    std::printf("PASS standalone Vulkan %s: %u physical device(s), all handles destroyed\n",device?"device":"instance",count);
    std::fflush(stdout); // Preserve completion evidence before LSan aborts.
    return 0;
}
int main(int argc,char** argv) {
    const bool device=argc==2 && !std::strcmp(argv[1],"--device");
    if(argc>1 && !device)return 2;
    unsigned repeats=1;
    if(const auto* value=std::getenv("KPXC_VULKAN_REPEATS")) {
        char* end=nullptr;auto count=std::strtoul(value,&end,10);
        if(!end || *end || count<1 || count>1000)return 2;
        repeats=unsigned(count);
    }
    // Optional diagnostic contrast: keep the already-installed driver loaded.
    // This changes module lifetime only in this control, never in the product.
    void* held=nullptr;
    if(const auto* library=std::getenv("KPXC_VULKAN_HOLD_DRIVER")) {
        held=dlopen(library,RTLD_NOW|RTLD_LOCAL);
        if(!held){std::fprintf(stderr,"Control dlopen failed: %s\n",dlerror());return 2;}
    }
    for(unsigned n=0;n<repeats;++n)if(auto status=run(device))return status;
    std::printf("PASS Vulkan repetitions=%u held_driver=%d\n",repeats,bool(held));std::fflush(stdout);
}
