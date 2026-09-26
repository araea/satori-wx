#pragma once

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

namespace wx {
// Only a readable, non-executable data page may be modified. No code patching.
template <typename T> class DataSlot {
public:
    bool install(T *slot, T replacement) {
        const long size = sysconf(_SC_PAGESIZE);
        if (!slot || size <= 0 || reinterpret_cast<uintptr_t>(slot) % alignof(T)) return false;
        slot_ = slot;
        size_ = static_cast<size_t>(size);
        page_ = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(slot) & ~(size_ - 1));
        FILE *maps = fopen("/proc/self/maps", "re");
        if (!maps) return false;
        char line[1024];
        uintptr_t begin, end;
        char perms[5];
        while (fgets(line, sizeof(line), maps)) {
            if (sscanf(line, "%lx-%lx %4s", &begin, &end, perms) != 3) continue;
            const uintptr_t address = reinterpret_cast<uintptr_t>(slot);
            if (address < begin || address >= end || end - address < sizeof(T)) continue;
            prot_ = (perms[0] == 'r' ? PROT_READ : 0) |
                    (perms[1] == 'w' ? PROT_WRITE : 0) |
                    (perms[2] == 'x' ? PROT_EXEC : 0);
            break;
        }
        fclose(maps);
        if (prot_ < 0 || !(prot_ & PROT_READ) || (prot_ & PROT_EXEC)) return false;
        original_ = __atomic_load_n(slot_, __ATOMIC_ACQUIRE);
        replacement_ = replacement;
        if (!original_ || original_ == replacement_) return false;
        if (mprotect(page_, size_, prot_ | PROT_WRITE) != 0) return false;
        T expected = original_;
        installed_ = __atomic_compare_exchange_n(slot_, &expected, replacement_, false,
                                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        protection_ok_ = mprotect(page_, size_, prot_) == 0;
        return installed_;
    }

    // Never overwrite another observer's pointer; never write after mprotect fails.
    // The module must stay resident even after success: a caller may have cached our wrapper.
    const char *restore() {
        if (!installed_) return "not-installed";
        if (mprotect(page_, size_, prot_ | PROT_WRITE) != 0) return "write-enable-failed";
        T expected = replacement_;
        const bool restored = __atomic_compare_exchange_n(slot_, &expected, original_, false,
                                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        protection_ok_ = mprotect(page_, size_, prot_) == 0;
        if (restored || expected != replacement_) installed_ = false;
        if (!protection_ok_) return "protection-restore-failed";
        return restored ? "restored" : "slot-changed";
    }

    T original() const { return original_; }
    bool protection_ok() const { return protection_ok_; }
private:
    T *slot_ = nullptr;
    T original_ = nullptr;
    T replacement_ = nullptr;
    void *page_ = nullptr;
    size_t size_ = 0;
    int prot_ = -1;
    bool installed_ = false;
    bool protection_ok_ = true;
};
} // namespace wx
