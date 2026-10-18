#pragma once
#include <stddef.h>

// Every name of WeChat's own code that the reflection senders need: classes (dotted), and the members
// (name and JNI descriptor) of the classes they call. WeChat is obfuscated and re-obfuscated on each
// release: the packages and most class names are shuffled again, a few service methods are renumbered,
// and the member names inside a class mostly stay. So nothing in the senders spells such a name; they
// ask this table.
//
// The table holds one SET of values per WeChat build. Built-in sets (8.0.78, 8.0.79) are in
// wx_names.cpp; wx_discover.cpp derives a set for a build nobody has looked at yet, straight from the
// installed APK, and installs it here. A sender resolves against the sets in order, newest first, and
// keeps the first one whose classes and members all exist; that is what "adapting" means at runtime.
//
// Class names that did not change between builds (and will probably not: they are the framework's and
// WeChat's own non-obfuscated ones) are in the table too, so that one place says what the code
// depends on.
namespace satori {
// X(key, "8.0.78 value", "8.0.79 value")
#define WX_NAME_LIST(X)                                                                                                  \
    /* --- sending text: the scene, its dispatcher and the callback it wants --- */                                      \
    X(SceneDispatcherCls, "com.tencent.mm.network.s", "com.tencent.mm.network.s")                                        \
    X(SceneCallbackCls, "com.tencent.mm.modelbase.u0", "com.tencent.mm.modelbase.u0")                                    \
    X(DoSceneDesc, "(Lcom/tencent/mm/network/s;Lcom/tencent/mm/modelbase/u0;)I",                                         \
      "(Lcom/tencent/mm/network/s;Lcom/tencent/mm/modelbase/u0;)I")                                                      \
    /* The calling thread's Looper: 8.0.78 builds Handlers that need one on the thread that dispatches;   */             \
    /* from 8.0.79 a thread that has one is worse than one that has none (see wx_send.cpp EnsureLooper).  */             \
    X(ThreadLooper, "prepare", "none")                                                                                   \
    X(SendSceneCls, "v51.r0", "f61.s0")                                                                                  \
    X(SendSceneLocalIdName, "f", "f")                                                                                    \
    X(NoopCallbackCls, "com.tencent.mm.network.y2", "com.tencent.mm.network.z2")                                         \
    X(PushCoreCls, "com.tencent.mm.network.a3", "com.tencent.mm.network.b3")                                             \
    X(PushCoreDispatcherName, "c", "c")                                                                                  \
    X(PushCoreDispatcherDesc, "()Lcom/tencent/mm/network/j1;", "()Lcom/tencent/mm/network/j1;")                          \
    X(NetQueueCls, "com.tencent.mm.modelbase.r1", "com.tencent.mm.modelbase.s1")                                         \
    X(NetQueueSingletonName, "y", "y")                                                                                   \
    X(NetQueueSingletonDesc, "Lcom/tencent/mm/modelbase/r1;", "Lcom/tencent/mm/modelbase/s1;")                           \
    X(NetQueueDispatcherName, "k", "k")                                                                                  \
    X(NetQueueDispatcherDesc, "()Lcom/tencent/mm/network/s;", "()Lcom/tencent/mm/network/s;")                            \
    /* --- recalling: the revoke scene and the message store it reads the message from --- */                            \
    X(RevokeCls, "com.tencent.mm.modelsimple.d1", "com.tencent.mm.modelsimple.d1")                                       \
    X(RevokeCtorDesc, "(Lcom/tencent/mm/storage/e9;Ljava/lang/String;Ljava/lang/String;)V",                              \
      "(Lcom/tencent/mm/storage/e9;Ljava/lang/String;Ljava/lang/String;)V")                                              \
    X(MsgInfoCls, "com.tencent.mm.storage.e9", "com.tencent.mm.storage.e9")                                              \
    /* Not obfuscated: a field of the message base class (im.c8), read rather than called. */                            \
    X(MsgInfoIsSendField, "field_isSend", "field_isSend")                                                                \
    X(StoreHolderCls, "ex0.k0", "ox0.k0")                                                                                \
    X(StoreHolderFieldName, "F0", "d1")                                                                                  \
    X(StoreHolderFieldDesc, "Lex0/j0;", "Lox0/j0;")                                                                      \
    X(StoreCls, "ex0.j0", "ox0.j0")                                                                                      \
    X(StoreGetName, "k", "k")                                                                                            \
    X(StoreGetDesc, "(Ljava/lang/String;J)Lcom/tencent/mm/storage/e9;",                                                  \
      "(Ljava/lang/String;J)Lcom/tencent/mm/storage/e9;")                                                                \
    /* --- the service registry every feature service is fetched from --- */                                             \
    X(LocatorCls, "ph5.n0", "ni5.n0")                                                                                    \
    X(LocatorGetName, "c", "c")                                                                                          \
    X(LocatorGetDesc, "(Ljava/lang/Class;)Lph5/m;", "(Ljava/lang/Class;)Lni5/m;")                                        \
    X(PrepareCls, "com.tencent.mm.pluginsdk.ui.tools.p0", "com.tencent.mm.pluginsdk.ui.tools.p0")                        \
    X(PrepareName, "a", "a")                                                                                             \
    /* --- pictures: the chat UI's own pipeline --- */                                                                   \
    X(ImageServiceCls, "kt.d1", "kt.d1")                                                                                 \
    X(ImageSendName, "rj", "Oj")                                                                                         \
    X(ImageSendDesc, "(Lda0/g;)Lkotlinx/coroutines/flow/j;", "(Lla0/g;)Lkotlinx/coroutines/flow/j;")                     \
    X(ImageParamsCls, "da0.g", "la0.g")                                                                                  \
    X(ImageParamsCtorDesc, "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Lw90/i0;)V",                         \
      "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Lea0/i0;)V")                                              \
    X(ImageParamsSourceName, "j", "k")                                                                                   \
    X(ImageContextCls, "w90.i0", "ea0.i0")                                                                               \
    X(ImageContextKindName, "a", "a")                                                                                    \
    X(ImageContextCallbackName, "o", "o")                                                                                \
    X(ImageContextCallbackDesc, "Lb41/k7;", "Ll41/k7;")                                                                  \
    X(ImageCallbackCls, "b41.k7", "l41.k7")                                                                              \
    /* --- files, quote replies and merged forwards: all through AppMsgLogic --- */                                      \
    X(AppMsgCls, "com.tencent.mm.pluginsdk.model.app.k0", "com.tencent.mm.pluginsdk.model.app.k0")                       \
    X(AppMsgAttachDirName, "k", "k")                                                                                     \
    X(AppMsgFreePathName, "f", "f")                                                                                      \
    X(AppMsgSendName, "I", "I")                                                                                          \
    X(AppMsgSendDesc,                                                                                                    \
      "(Ldx0/r;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)Landroid/util/Pair;",          \
      "(Lnx0/r;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)Landroid/util/Pair;")          \
    X(ContentCls, "dx0.r", "nx0.r")                                                                                      \
    X(ContentParseName, "v", "v")                                                                                        \
    X(ContentParseDesc, "(Ljava/lang/String;)Ldx0/r;", "(Ljava/lang/String;)Lnx0/r;")                                    \
    X(ContentTitleName, "f", "f")                                                                                        \
    X(ContentKindName, "i", "i")                                                                                         \
    X(ContentQuoteName, "x2", "x2")                                                                                      \
    X(QuoteItemCls, "com.tencent.mm.plugin.msgquote.model.MsgQuoteItem",                                                 \
      "com.tencent.mm.plugin.msgquote.model.MsgQuoteItem")                                                               \
    X(QuoteItemTypeName, "d", "d")                                                                                       \
    X(QuoteItemSvrName, "e", "e")                                                                                        \
    X(QuoteItemFromName, "f", "f")                                                                                       \
    X(QuoteItemChatName, "g", "g")                                                                                       \
    X(QuoteItemDisplayName, "h", "h")                                                                                    \
    X(QuoteItemSourceName, "i", "i")                                                                                     \
    X(QuoteItemContentName, "m", "m")                                                                                    \
    X(QuoteItemMergedName, "n", "n")                                                                                     \
    X(QuoteItemStridName, "p", "p")                                                                                      \
    X(QuoteItemCreatedName, "q", "q")                                                                                    \
    /* --- video: the video feature service --- */                                                                       \
    X(VideoServiceCls, "ab5.s", "yb5.s")                                                                                 \
    X(VideoSendName, "cj", "zj")                                                                                         \
    X(VideoSendDesc, "(Lqi0/w2;Ljava/lang/String;)Z", "(Lyi0/w2;Ljava/lang/String;)Z")                                   \
    X(VideoElementCls, "qi0.w2", "yi0.w2")                                                                               \
    X(VideoElementCtorDesc, "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ZILqi0/t2;Lb41/k7;)V",               \
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ZILyi0/t2;Ll41/k7;)V")                                     \
    X(VideoCrossCls, "qi0.t2", "yi0.t2")                                                                                 \
    X(VideoCrossCtorDesc,                                                                                                \
      "(Lb41/i7;Lpc5/qn6;Ljava/lang/String;Lpc5/p87;Ljava/lang/String;Lpc5/qn4;ZLqi0/r2;Ljava/lang/String;Lp95/f;ZZZ)V", \
      "(Ll41/i7;Lnd5/fr6;Ljava/lang/String;Lnd5/hc7;Ljava/lang/String;Lnd5/gq4;ZLyi0/r2;Ljava/lang/String;Lna5/f;ZZZ)V") \
    /* --- voice messages: VoiceLogic, the path service and the uploader --- */                                          \
    X(VoiceLogicCls, "v61.d1", "f71.d1")                                                                                 \
    X(VoiceStartName, "h", "h")                                                                                          \
    X(VoiceFinishName, "u", "u")                                                                                         \
    X(VoiceFinishDesc, "(Ljava/lang/String;IILcom/tencent/mm/storage/e9;Ljava/lang/String;)Z",                           \
      "(Ljava/lang/String;IILcom/tencent/mm/storage/e9;Ljava/lang/String;)Z")                                            \
    X(VoicePathsCls, "rn3.u0", "no3.u0")                                                                                 \
    X(VoicePathName, "Fj", "ck")                                                                                         \
    X(VoicePathDesc, "(Lou5/x;Ljava/lang/String;ZZ)Ljava/lang/String;",                                                  \
      "(Lpv5/x;Ljava/lang/String;ZZ)Ljava/lang/String;")                                                                 \
    X(VoiceKindCls, "ou5.x", "pv5.x")                                                                                    \
    X(VoiceKindFieldName, "j", "j")                                                                                      \
    X(VoiceKindFieldDesc, "Lou5/x;", "Lpv5/x;")                                                                          \
    X(VoiceSubCoreCls, "v61.v0", "f71.v0")                                                                               \
    X(VoiceUploaderOfName, "dj", "Aj")                                                                                   \
    X(VoiceUploaderOfDesc, "()Lyl/y0;", "()Lyl/z0;")                                                                     \
    X(VoiceUploaderCls, "yl.y0", "yl.z0")                                                                                \
    X(VoiceUploaderRunName, "e", "e")                                                                                    \
    /* --- group management --- */                                                                                       \
    X(RoomKickCls, "qn.p", "qn.q")                                                                                       \
    X(RoomAdminAddCls, "qn.b", "qn.b")                                                                                   \
    X(RoomAdminDelCls, "qn.e", "qn.f")                                                                                   \
    X(RoomCgiBaseCls, "com.tencent.mm.modelbase.i", "com.tencent.mm.modelbase.i")                                        \
    X(RoomCgiReqName, "f", "f")                                                                                          \
    X(RoomCgiReqDesc, "Lcom/tencent/mm/modelbase/o;", "Lcom/tencent/mm/modelbase/o;")                                    \
    X(RoomRunnerCls, "com.tencent.mm.modelbase.z2", "com.tencent.mm.modelbase.a3")                                       \
    X(RoomRunnerName, "d", "d")                                                                                          \
    X(RoomRunnerDesc, "(Lcom/tencent/mm/modelbase/o;Lcom/tencent/mm/modelbase/e3;Z)Lcom/tencent/mm/modelbase/m1;",       \
      "(Lcom/tencent/mm/modelbase/o;Lcom/tencent/mm/modelbase/f3;Z)Lcom/tencent/mm/modelbase/n1;")                       \
    /* --- pat (拍一拍) --- */                                                                                           \
    X(PatServiceCls, "ov3.j", "kw3.j")                                                                                   \
    X(PatManagerCls, "nv3.l", "jw3.l")                                                                                   \
    X(PatInsertName, "nj", "Kj")                                                                                         \
    X(PatSceneCls, "qv3.b", "mw3.b")

enum WxKey {
#define X(key, v78, v79) k##key,
    WX_NAME_LIST(X)
#undef X
        kWxKeyCount
};

struct WxNames {
    const char *label;              // "8.0.79", "discovered"
    const char *value[kWxKeyCount]; // every key has a value; the sets never have holes
};

// The sets a sender tries, best first: a discovered set when the engine found one for this build,
// then the built-in sets, newest build first. Indices are stable for the life of the process
// except that a discovered set may appear at index 0 (WxNamesInstall).
int WxNamesCount();
const WxNames &WxNamesAt(int index);
// Takes a discovered set (copied; `values` holds kWxKeyCount strings in key order). A later call
// replaces the earlier one. Returns false when a value is missing.
bool WxNamesInstall(const char *label, const char *const *values);
// The key's identifier ("SendSceneCls"), for diagnostics and the cache file.
const char *WxKeyId(WxKey key);
// The key with this identifier, or -1.
int WxKeyByName(const char *id);
// Class names are dotted for ClassLoader.loadClass; JNI descriptors want slashes.
// Writes "a/b/C" for "a.b.C" (truncating if it does not fit) and returns `out`.
const char *WxSlash(const char *dotted, char *out, size_t size);
// "La/b/C;" for the dotted class name.
const char *WxClassDesc(const char *dotted, char *out, size_t size);
} // namespace satori
