#include <stdio.h>
#include <string>

#include "Utils/PtraceUtils.h"

int injectRemoteProcess();

const char *libraryPath = nullptr;
int pid = 0;

int initInject() {
    int res = injectRemoteProcess();
    LOGW("Inject Result: %d", res);
    return res;
}

int callRemoteMmap(pt_regs regs) {
    long parameters[1];

    void *mmapAddr = getRemoteFuncAddr(pid, libcPath, (void *)malloc);
    LOGI("Mmap Function Address: 0x%lx\n", (uintptr_t)mmapAddr);

    //void *mmap(void *start, size_t length, int prot, int flags, int fd, off_t offsize);
    //parameters[0] = 0; //Not needed
    //parameters[1] = 0x3000;
    //parameters[2] = PROT_READ | PROT_WRITE | PROT_EXEC;
    //parameters[3] = MAP_ANONYMOUS | MAP_PRIVATE;
    //parameters[4] = 0; //Not needed
    //parameters[5] = 0; //Not needed
	
	//This hopefully fixes I/O Error issues
    //void* malloc(size_t size)
    parameters[0] = 256; //Size (I don't think any path will be longer than that)

    //Call the mmap function of the target process
    return ptrace_call(pid, (uintptr_t)mmapAddr, parameters, 6, &regs);
}

int callRemoteDlopen(pt_regs regs, void *remoteMmapAddr) {
    long parameters[2];

    void *dlopen_addr = getDlOpenAddr(pid);
    LOGE("dlopen getRemoteFuncAddr: 0x%lx", (uintptr_t)dlopen_addr);

    //Get address for dlerror
    void *dlErrorAddr = getDlerrorAddr(pid);

    //Return value of dlopen is the start address of the loaded module
    //void *dlopen(const char *filename, int flag);
    parameters[0] = (uintptr_t) remoteMmapAddr;
    parameters[1] = RTLD_NOW | RTLD_GLOBAL;

    //Calls dlopen which loads the lib
    if (ptrace_call(pid, (uintptr_t) dlopen_addr, parameters, 2, &regs) != -1) {
        LOGE("Call dlopen Failed");
        return -1;
    }

    void *remoteModuleAddr = (void *)ptrace_getret(&regs);
    LOGI("ptrace_call dlopen success, Remote module Address: 0x%lx", (long)remoteModuleAddr);

    //dlopen error
    if ((long) remoteModuleAddr == 0x0) {
        LOGE("dlopen error");
        if (ptrace_call(pid, (uintptr_t) dlErrorAddr, parameters, 0, &regs) == -1) {
            LOGE("Call dlerror failed");
            return -1;
        }
        char *error = (char *) ptrace_getret(&regs);
        char localErrorInfo[1024] = {0};
        ptrace_readdata(pid, (uint8_t *) error, (uint8_t *) localErrorInfo, 1024);
        LOGE("dlopen error: %s\n", localErrorInfo);
        return -1;
    }
    return 0;
}

int injectRemoteProcess() {
    //Instead of directly returning we use a value so if something fails we still detach from the process
    int returnValue = 0;

    //Attach to the target proc
    if (ptraceAttach(pid) != 0) {
        return -1;
    }

    struct pt_regs currentRegs, originalRegs;
    if (ptrace_getregs(pid, &currentRegs) != 0) {
        LOGE("Ptrace getregs failed");
        return -1;
    }

    //Backup Original Register
    memcpy(&originalRegs, &currentRegs, sizeof(currentRegs));

    if (callRemoteMmap(currentRegs) == -1) {
        LOGE("Call Remote mmap Func Failed: %s", strerror(errno));
        returnValue = -1;
    }
    LOGE("ptrace_call mmap success. return value=%lX, pc=%lX", ptrace_getret(&currentRegs), ptrace_getpc(&currentRegs));

    // Return value is the starting address of the memory map
    void *remoteMapMemoryAddr = (void *)ptrace_getret(&currentRegs);
    LOGI("Remote Process Map Address: 0x%lx", (uintptr_t)remoteMapMemoryAddr);

    //Params:            pid,             start addr,                      content,          size
    if (ptrace_writedata(pid, (uint8_t *) remoteMapMemoryAddr, (uint8_t *) libraryPath, strlen(libraryPath) + 1) == -1) {
        LOGE("writing %s to process failed", libraryPath);
        returnValue = -1;
    }

    if (callRemoteDlopen(currentRegs, remoteMapMemoryAddr) == -1) {
        LOGE("Call dlopen Failed");
        returnValue = -1;
    }

    if (ptrace_setregs(pid, &originalRegs) == -1) {
        LOGE("Could not recover reges");
        returnValue = -1;
    }

    ptrace_getregs(pid, &currentRegs);
    if (memcmp(&originalRegs, &currentRegs, sizeof(currentRegs)) != 0) {
        LOGE("Set Regs Error");
        returnValue = -1;
    }

    ptraceDetach(pid);

    return returnValue;
}

int main(int argc, char const *argv[]) {
    if (argc < 3) return -1;
    LOGI("Start Injecting");
    LOGI("PID: %s", argv[1]);
    LOGI("Library Path: %s", argv[2]);
    libraryPath = argv[2];
    pid = atoi(argv[1]);

    if (initInject() == 0) {
        LOGI("Injection Complete\n");
		return 1;
    }
    else {
        LOGE("Injection Failed\n");
		return 0;
    }
}
