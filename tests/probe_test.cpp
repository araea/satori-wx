// Include the implementation to exercise its ownership/concurrency boundary without ART.
#include "../native/probe.cpp"
#include <assert.h>

namespace {
long refs = 0;
thread_local bool exception = false;
bool fail_ref = false;
jint original_result = JNI_OK;
jobject NewRef(JNIEnv *, jobject obj) {
    if (fail_ref) { exception = true; return nullptr; }
    __atomic_add_fetch(&refs, 1, __ATOMIC_RELAXED); return obj;
}
void DeleteRef(JNIEnv *, jobject) { __atomic_sub_fetch(&refs, 1, __ATOMIC_RELAXED); }
jboolean HasException(JNIEnv *) { return exception; }
void ClearException(JNIEnv *) { exception = false; }
jint Original(JNIEnv *, jclass, const JNINativeMethod *, jint) { return original_result; }
jint Other(JNIEnv *, jclass, const JNINativeMethod *, jint) { return 7; }
JNINativeInterface functions{};
JNINativeMethod method = {"example", "()V", reinterpret_cast<void *>(Original)};
void *Produce(void *) {
    JNIEnv env{&functions};
    for (int i = 0; i < 300; ++i)
        assert(ObserveRegister(&env, reinterpret_cast<jclass>(1), &method, 1) == JNI_OK);
    return nullptr;
}
}
int main() {
    functions.NewGlobalRef = NewRef; functions.DeleteGlobalRef = DeleteRef;
    functions.ExceptionCheck = HasException; functions.ExceptionClear = ClearException;
    JNIEnv env{&functions};
    const size_t page = sysconf(_SC_PAGESIZE);
    auto *slot = static_cast<RegisterFn *>(mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(slot != MAP_FAILED);
    *slot = Original;
    assert(mprotect(slot, page, PROT_READ) == 0);
    assert(g_patch.install(slot, ObserveRegister));
    assert(*slot == ObserveRegister && g_patch.original() == Original && g_patch.protection_ok());
    g_active = true;
    pthread_t threads[4];
    for (auto &thread : threads) assert(pthread_create(&thread, nullptr, Produce, nullptr) == 0);
    for (auto &thread : threads) pthread_join(thread, nullptr);
    assert(g_calls == 1200 && g_methods == 1200 && g_drops == 1200 - kCapacity);
    assert(refs == kCapacity);
    assert(strcmp(Stop(), "restored") == 0 && *slot == Original);
    // A stale function pointer remains callable after restore, but cannot enqueue.
    const long calls = g_calls;
    assert(ObserveRegister(&env, reinterpret_cast<jclass>(1), &method, 1) == JNI_OK);
    assert(g_calls == calls);
    Entry entry{}; char name[32];
    while (Pop(&entry)) Resolve(&env, nullptr, entry, name, sizeof(name));
    assert(refs == 0);
    g_active = true; fail_ref = true;
    assert(ObserveRegister(&env, reinterpret_cast<jclass>(1), &method, 1) == JNI_OK);
    assert(!exception && refs == 0 && g_count == 0);
    fail_ref = false; exception = true;
    assert(ObserveRegister(&env, reinterpret_cast<jclass>(1), &method, 1) == JNI_OK);
    assert(exception && refs == 0); exception = false;
    original_result = JNI_ERR;
    assert(ObserveRegister(&env, reinterpret_cast<jclass>(1), &method, 1) == JNI_ERR && refs == 0);
    original_result = JNI_OK;
    assert(Printed(4000, 32) == 31 && Printed(-1, 32) == 0);
    wx::DataSlot<RegisterFn> conflict;
    assert(conflict.install(slot, ObserveRegister));
    assert(mprotect(slot, page, PROT_READ | PROT_WRITE) == 0);
    *slot = Other;
    assert(mprotect(slot, page, PROT_READ) == 0);
    assert(strcmp(conflict.restore(), "slot-changed") == 0 && *slot == Other);
    assert(mprotect(slot, page, PROT_READ | PROT_EXEC) == 0);
    wx::DataSlot<RegisterFn> executable;
    assert(!executable.install(slot, ObserveRegister));
    assert(mprotect(slot, page, PROT_READ | PROT_WRITE) == 0);
    *slot = Original;
    wx::DataSlot<RegisterFn> unmapped;
    assert(unmapped.install(slot, ObserveRegister));
    assert(munmap(slot, page) == 0);
    assert(strcmp(unmapped.restore(), "write-enable-failed") == 0);
    puts("probe tests passed: queue/ref ownership, concurrency, exceptions, page protections, restore conflicts");
}
