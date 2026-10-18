#!/ usr / bin / env python3
""
    "Derives native/wx_names.h's values for a WeChat build from its APK: the reference for the on-device
    engine(native / wx_discover.cpp),
    which follows the same rules.

        WeChat is re
        - obfuscated on every release,
    so the obfuscated class names move.What stays put is what the code says : log tags, cgi paths,
    the shape of constructors and methods,
    and who points at whom.The rules below start from such anchors, walk the relations to the other classes,
    and read member names and descriptors off the dex.A member name is taken from the previous
        build's table ("hint") only to choose between candidates of the same shape,
    never to invent one
        .

    usage : wxnames.py<apk>[--check] print the derived set; --check also compares it with the built-in sets
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wxindex  # noqa: E402

HEADER = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'native', 'wx_names.h')


def table(path=HEADER):
    """{label: {key: value}} for the built-in sets of wx_names.h."""
    text = open(path).read()
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    text = re.sub(r'\\\n', ' ', text)
    out = {'8.0.78': {}, '8.0.79': {}}
    order = []

    def lit(s):
        return ''.join(re.findall(r'"([^"]*)"', s))

    for m in re.finditer(r'X\((\w+),\s*((?:"[^"]*"\s*)+),\s*((?:"[^"]*"\s*)+)\)', text):
        order.append(m.group(1))
        out['8.0.78'][m.group(1)] = lit(m.group(2))
        out['8.0.79'][m.group(1)] = lit(m.group(3))
    return out, order


def dotted(desc):
    return desc[1:-1].replace('/', '.')


def descriptor(name):
    return 'L' + name.replace('.', '/') + ';'


def params(proto):
    """Parameter types of a method descriptor, each a type descriptor."""
    out = []
    i = 1
    while proto[i] != ')':
        j = i
        while proto[j] == '[':
            j += 1
        j = proto.index(';', j) + 1 if proto[j] == 'L' else j + 1
        out.append(proto[i:j])
        i = j
    return out


def ret(proto):
    return proto[proto.index(')') + 1:]


_shapes = {}


def shape(pattern, proto):
    """True when `proto` matches `pattern`, where `L*;` stands for any class type."""
    rx = _shapes.get(pattern)
    if rx is None:
        out = ''
        i = 0
        while i < len(pattern):
            if pattern.startswith('L*;', i):
                out += r'L[^;]*;'
                i += 3
            else:
                out += re.escape(pattern[i])
                i += 1
        rx = _shapes[pattern] = re.compile(out)
    return rx.fullmatch(proto) is not None


STATIC, FINAL = 0x8, 0x10


class Finder:
    def __init__(self, index, hints):
        self.c = index['classes']
        self.a = index['anchors']
        self.hints = hints  # {key: value} of the newest built-in set
        self.r = {}
        self.why = {}

#-- queries -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
    def anchor(self, text):
        """{class descriptor: [(method name, proto)]} for the classes that load this string constant."""
        return self.a.get(text, {})

    def methods(self, cls, name=None, pattern=None, static=None):
        for n, p, f in self.c[cls]['methods'] if cls in self.c else ():
            if name is not None and n != name:
                continue
            if pattern is not None and not shape(pattern, p):
                continue
            if static is not None and bool(f & STATIC) != static:
                continue
            yield n, p, f

    def fields(self, cls, type_=None, static=None):
        for n, t, f in self.c[cls]['fields'] if cls in self.c else ():
            if type_ is not None and t != type_:
                continue
            if static is not None and bool(f & STATIC) != static:
                continue
            yield n, t, f

    def supers(self, cls):
        """The class, its super classes and every interface on the way."""
        seen = []
        todo = [cls]
        while todo:
            c = todo.pop()
            if c in seen or c not in self.c:
                continue
            seen.append(c)
            info = self.c[c]
            todo.append(info['super'])
            todo.extend(info['ifaces'])
        return seen

    def is_a(self, cls, target):
        return target in self.supers(cls)

    def pick(self, key, candidates):
        """One of `candidates` (names), preferring the previous build's name when it is among them."""
        names = sorted(set(candidates))
        if len(names) == 1:
            return names[0]
        hint = self.hints.get(key)
        if hint in names:
            return hint
        return None

#-- results -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
    def set(self, key, value):
        self.r[key] = value
        return value

    def fail(self, key, reason):
        self.why[key] = reason
        return None

    def cls_key(self, key, desc):
        return self.set(key, dotted(desc))

#-- the rules -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --
    def run(self):
        for step in (self.send, self.recall, self.locator, self.prepare, self.image, self.appmsg, self.video,
                     self.voice, self.room, self.pat):
            step()
        return self.r

    def service_of(self, impl):
        """The service interface an implementation class is registered under: the interface that is itself
        a service (reaches the type the locator hands out)."""
        root = ret(self.r['LocatorGetDesc']) if 'LocatorGetDesc' in self.r else None
        found = [i for i in self.c[impl]['ifaces'] if root and self.is_a(i, root) and i != root]
        return found[0] if len(found) == 1 else None

    def member(self, key, desc_key, cls, kind, pattern, static=None, up=False, fallback=None):
        """Name (and descriptor) of the one member of `cls` that has this shape; the previous build's name
        decides between several. kind is 'm' (method) or 'f' (field)."""
        if cls is None:
            return self.fail(key, 'class not found')
        found = []
        for c in (self.supers(cls) if up else [cls]):
            if kind == 'm':
                found += [(n, p) for n, p, f in self.methods(c, pattern=pattern, static=static)]
            else:
                found += [(n, t) for n, t, f in self.fields(c, static=static) if shape(pattern, t)]
        names = [n for n, _ in found]
        name = self.pick(key, names)
        if name is None and fallback == 'first' and names:
            name = names[0]
        if name is None and fallback == 'last' and names:
            name = names[-1]
        if name is None:
            return self.fail(key, 'no member' if not names else 'ambiguous: ' + ','.join(sorted(set(names))))
        shapes = sorted({d for n, d in found if n == name})
        if len(shapes) != 1:
            return self.fail(key, 'overloaded: ' + ','.join(shapes))
        self.set(key, name)
        if desc_key:
            self.set(desc_key, shapes[0])
        return shapes[0]

    def ctor(self, cls, pattern=None):
        """The constructor of `cls` with this shape, or its real constructor (not the Kotlin default-argument
        bridge, which ends in kotlin.jvm.internal.*) when there is one."""
        found = [p for n, p, f in self.methods(cls, '<init>', pattern)
                 if not params(p) or not params(p)[-1].startswith('Lkotlin/jvm/internal/')]
        return found[0] if len(found) == 1 else None

    def by_proto(self, proto):
        if not hasattr(self, '_by_proto'):
            self._by_proto = {}
            for c, info in self.c.items():
                for n, p, f in info['methods']:
                    self._by_proto.setdefault(p, []).append((c, n, f))
        return self._by_proto.get(proto, [])

    def static_fields_of(self, type_):
        if not hasattr(self, '_by_ftype'):
            self._by_ftype = {}
            for c, info in self.c.items():
                for n, t, f in info['fields']:
                    if f & STATIC:
                        self._by_ftype.setdefault(t, []).append((c, n, f))
        return self._by_ftype.get(type_, [])

    def send(self):
#doScene(dispatcher, callback) is declared on the scene base class; the cgi base and the text scene
#both lead there.
        found = set()
        for start in list(self.anchor('MicroMsg.Cgi')) + list(self.anchor('MicroMsg.NetSceneSendMsg')):
            for c in self.supers(start):
                found |= {p for n, p, f in self.methods(c, 'doScene', '(L*;L*;)I')
}
if len (found) == 1:
            desc = next(iter(found))
            self.set('DoSceneDesc', desc)
            self.cls_key('SceneDispatcherCls', params(desc)[0])
            self.cls_key('SceneCallbackCls', params(desc)[1])
#The text scene : the class that logs as NetSceneSendMsg and takes(talker, text, type, flag, extra, source).
        scene = None
        for c in self.anchor('MicroMsg.NetSceneSendMsg'):
            if any(True for _ in self.methods(c, '<init>', '(Ljava/lang/String;Ljava/lang/String;IIJLjava/lang/String;)V')):
                scene = c
        if not scene:
            return self.fail('SendSceneCls', 'no NetSceneSendMsg with the text constructor')
        self.cls_key('SendSceneCls', scene)
        self.member('SendSceneLocalIdName', None, scene, 'f', 'J', static=False)
        callback = descriptor(self.r.get('SceneCallbackCls', ''))
#The callback object handed to doScene : the class that logs "Run successfully" and implements the callback.
        for c in self.anchor('[+] Run successfully: %s, funcId: %s'):
            if self.is_a(c, callback):
                self.cls_key('NoopCallbackCls', c)
#The push core, whose static accessor returns the remote dispatcher.
        dispatcher = descriptor(self.r.get('SceneDispatcherCls', ''))
        for c in self.anchor('[+] MMAutoAuth was injected, run this scene now: %s, funcId: %s'):
            self.cls_key('PushCoreCls', c)
            found = [(n, p) for n, p, f in self.methods(c, pattern='()L*;', static=True) if self.is_a(ret(p), dispatcher)]
            if len(found) == 1:
                self.set('PushCoreDispatcherName', found[0][0])
                self.set('PushCoreDispatcherDesc', found[0][1])
#The net queue : holds the dispatcher and a static instance of itself.
        for c in self.anchor('MicroMsg.NetSceneQueue'):
            singleton = [(n, t) for n, t, f in self.fields(c, c, static=True)]
            getter = [(n, p) for n, p, f in self.methods(c, pattern='()' + dispatcher, static=False)]
            if len(singleton) == 1 and len(getter) == 1:
                self.cls_key('NetQueueCls', c)
                self.set('NetQueueSingletonName', singleton[0][0])
                self.set('NetQueueSingletonDesc', singleton[0][1])
                self.set('NetQueueDispatcherName', getter[0][0])
                self.set('NetQueueDispatcherDesc', getter[0][1])

    def recall(self):
#The revoke scene : the cgi path plus a constructor that takes(message, ?, ?).
        for c, ms in self.anchor('/cgi-bin/micromsg-bin/revokemsg').items():
            for n, p, f in self.methods(c, '<init>', '(L*;Ljava/lang/String;Ljava/lang/String;)V'):
                self.cls_key('RevokeCls', c)
                self.set('RevokeCtorDesc', p)
                self.cls_key('MsgInfoCls', params(p)[0])
        msg = descriptor(self.r['MsgInfoCls']) if 'MsgInfoCls' in self.r else None
#Whether the message was sent by this account is a field of the message base class(im.c8,
#a library class that keeps its names), not a method : read it instead of calling one.
        self.set('MsgInfoIsSendField', 'field_isSend')
        if not msg:
            return
#The message store : a class with several final(talker, id) -> message getters, that another class
#(the holder, an interface of the service kind) keeps one static instance of.
        getter = '(Ljava/lang/String;J)' + msg
        stores = {}
        for c, n, f in self.by_proto(getter):
            if not f & STATIC and f & FINAL:
                stores.setdefault(c, []).append(n)
        stores = [c for c, ns in stores.items() if len(ns) >= 2 and any(h != c for h, _, _ in self.static_fields_of(c))]
        if len(stores) != 1:
            return self.fail('StoreCls', 'store candidates: %d' % len(stores))
        store = stores[0]
        self.cls_key('StoreCls', store)
        holders = [(h, n) for h, n, f in self.static_fields_of(store) if h != store]
        if len(holders) == 1:
            self.cls_key('StoreHolderCls', holders[0][0])
            self.set('StoreHolderFieldName', holders[0][1])
            self.set('StoreHolderFieldDesc', store)
        self.member('StoreGetName', 'StoreGetDesc', store, 'm', getter, static=False)

    def locator(self):
#The service registry : logs as ServiceManager and has the static lookup(Class)->service.
        manager = self.anchor('MicroMsg.ServiceManager')
        best = None
        for c, ms in manager.items():
            found = [p for n, p, f in self.methods(c, pattern='(Ljava/lang/Class;)L*;', static=True) if ret(p) != 'Z']
            if len(found) == 1 and (best is None or len(ms) > len(manager[best])):
                best = c
        if best:
            self.cls_key('LocatorCls', best)
            self.member('LocatorGetName', 'LocatorGetDesc', best, 'm', '(Ljava/lang/Class;)L*;', static=True)

    def prepare(self):
#The chat UI's one-time preparation: a static ()V on the class that logs as BizHalfScreenChattingUtil.
        found = [c for c in self.anchor('MicroMsg.BizHalfScreenChattingUtil')
                 if any(n != '<clinit>' for n, _, _ in self.methods(c, pattern='()V', static=True))
                 and any(True for _ in self.methods(c, '<clinit>'))]
        if len(found) == 1:
            self.cls_key('PrepareCls', found[0])
            self.member('PrepareName', None, found[0], 'm', '()V', static=True)

    def image(self):
        flow = '(L*;)Lkotlinx/coroutines/flow/*;'
        impls = [c for c in self.anchor('MicroMsg.ImgUpload.MsgImgFeatureService')
                 if sum(1 for n, p, f in self.methods(c, static=False) if re.fullmatch(r'\(L[^;]*;\)Lkotlinx/coroutines/flow/[^;]*;', p)) == 1]
        if len(impls) != 1:
            return self.fail('ImageServiceCls', 'image service candidates: %d' % len(impls))
        impl = impls[0]
        service = self.service_of(impl)
        if service:
            self.cls_key('ImageServiceCls', service)
        for n, p, f in self.methods(impl, static=False):
            if re.fullmatch(r'\(L[^;]*;\)Lkotlinx/coroutines/flow/[^;]*;', p):
                self.set('ImageSendName', n)
                self.set('ImageSendDesc', p)
                self.cls_key('ImageParamsCls', params(p)[0])
        params_cls = descriptor(self.r['ImageParamsCls']) if 'ImageParamsCls' in self.r else None
        if not params_cls:
            return
        ctor = self.ctor(params_cls, '(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;L*;)V')
        if ctor:
            self.set('ImageParamsCtorDesc', ctor)
            self.cls_key('ImageContextCls', params(ctor)[4])
#The field the constructor fills with "msg_raw_img_send" is the source tag.
        source = [f for c, ms in self.anchor('msg_raw_img_send').items() if c == params_cls for m in ms for f in [m[2]] if f]
        if len(set(source)) == 1:
            self.set('ImageParamsSourceName', source[0])
        else:
            self.member('ImageParamsSourceName', None, params_cls, 'f', 'Ljava/lang/String;', static=False, fallback='last')
        for c in self.anchor('ForwardInfo{forwardFromImgId='):
            self.cls_key('ImageCallbackCls', c)
        context = descriptor(self.r['ImageContextCls']) if 'ImageContextCls' in self.r else None
        if context:
            self.member('ImageContextKindName', None, context, 'f', 'I', static=False, fallback='first')
            if 'ImageCallbackCls' in self.r:
                self.member('ImageContextCallbackName', 'ImageContextCallbackDesc', context, 'f',
                            descriptor(self.r['ImageCallbackCls']), static=False)

    def appmsg(self):
        send = '(L*;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)Landroid/util/Pair;'
        logic = [c for c in self.anchor('MicroMsg.AppMsgLogic') if any(True for _ in self.methods(c, pattern=send, static=True))]
        if len(logic) != 1:
            return self.fail('AppMsgCls', 'app message candidates: %d' % len(logic))
        logic = logic[0]
        self.cls_key('AppMsgCls', logic)
        self.member('AppMsgAttachDirName', None, logic, 'm', '()Ljava/lang/String;', static=True)
        self.member('AppMsgFreePathName', None, logic, 'm',
                    '(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;', static=True)
        desc = self.member('AppMsgSendName', 'AppMsgSendDesc', logic, 'm', send, static=True)
        if not desc:
            return
        content = params(desc)[0]
        self.cls_key('ContentCls', content)
        self.member('ContentParseName', 'ContentParseDesc', content, 'm', '(Ljava/lang/String;)' + content, static=True)
        self.member('ContentTitleName', None, content, 'f', 'Ljava/lang/String;', static=False)
        self.member('ContentKindName', None, content, 'f', 'I', static=False)
        hint = self.hints.get('QuoteItemCls')
        quote = [t for n, t, f in self.fields(content, static=False) if hint and t == descriptor(hint)]
        if not quote:
            quote = [t for n, t, f in self.fields(content, static=False) if t.endswith('/MsgQuoteItem;')]
        if len(quote) == 1:
            self.cls_key('QuoteItemCls', quote[0])
            self.member('ContentQuoteName', None, content, 'f', quote[0], static=False)
            for key, type_ in (('Type', 'I'), ('Svr', 'J'), ('From', 'Ljava/lang/String;'), ('Chat', 'Ljava/lang/String;'),
                               ('Display', 'Ljava/lang/String;'), ('Source', 'Ljava/lang/String;'),
                               ('Content', 'Ljava/lang/String;'), ('Merged', 'Ljava/lang/String;'),
                               ('Strid', 'Ljava/lang/String;'), ('Created', 'J')):
                self.member('QuoteItem%sName' % key, None, quote[0], 'f', type_, static=False)

    def video(self):
        shape_ = '(L*;Ljava/lang/String;)Z'
        impls = [c for c in self.anchor('MicroMsg.VideoMsg.VideoMsgSendFeatureService')
                 if sum(1 for _ in self.methods(c, pattern=shape_, static=False)) == 1 and self.service_of(c)]
        if len(impls) != 1:
            return self.fail('VideoServiceCls', 'video service candidates: %d' % len(impls))
        self.cls_key('VideoServiceCls', self.service_of(impls[0]))
        desc = self.member('VideoSendName', 'VideoSendDesc', impls[0], 'm', shape_, static=False)
        if not desc:
            return
        element = params(desc)[0]
        self.cls_key('VideoElementCls', element)
        ctor = self.ctor(element, '(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ZIL*;L*;)V')
        if not ctor:
            return
        self.set('VideoElementCtorDesc', ctor)
        cross = params(ctor)[5]
        self.cls_key('VideoCrossCls', cross)
        cross_ctor = self.ctor(cross)
        if cross_ctor:
            self.set('VideoCrossCtorDesc', cross_ctor)

    def voice(self):
        both = set(self.anchor('MicroMsg.SubCoreVoice')) & set(self.anchor('MicroMsg.SceneVoiceService'))
        if len(both) == 1:
            sub = next(iter(both))
            self.cls_key('VoiceSubCoreCls', sub)
            uploaders = self.anchor('MicroMsg.SceneVoiceService')
            found = [(n, p) for n, p, f in self.methods(sub, pattern='()L*;', static=True) if ret(p) in uploaders and ret(p) != sub]
            if len(found) == 1:
                self.set('VoiceUploaderOfName', found[0][0])
                self.set('VoiceUploaderOfDesc', found[0][1])
                self.cls_key('VoiceUploaderCls', ret(found[0][1]))
                self.member('VoiceUploaderRunName', None, ret(found[0][1]), 'm', '()V', static=False)
        msg = descriptor(self.r['MsgInfoCls']) if 'MsgInfoCls' in self.r else None
        if msg:
            finish = [(c, n, f) for c, n, f in self.by_proto('(Ljava/lang/String;IIL' + msg[1:] + 'Ljava/lang/String;)Z') if f & STATIC]
            if len(finish) == 1:
                self.cls_key('VoiceLogicCls', finish[0][0])
                self.set('VoiceFinishName', finish[0][1])
                self.set('VoiceFinishDesc', '(Ljava/lang/String;IIL' + msg[1:] + 'Ljava/lang/String;)Z')
                self.member('VoiceStartName', None, finish[0][0], 'm', '(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;', static=True)
        impls = list(self.anchor('MicroMsg.C2CFileFeatureService'))
        if len(impls) == 1 and self.service_of(impls[0]):
            self.cls_key('VoicePathsCls', self.service_of(impls[0]))
#The path method takes(kind, String, boolean, boolean) and builds a file name.Several
#methods in the service have that shape(the message and the app - message variants), so the
#kind class itself has to say which one : it is the one that declares a static instance of
#its own type, next to its variant enum.
            picks = []
            for n, p, f in self.methods(impls[0], pattern='(L*;Ljava/lang/String;ZZ)Ljava/lang/String;', static=False):
                first = params(p)[0]
                if any(True for _ in self.fields(first, type_=first, static=True)):
                    picks.append((n, p, first))
            kinds = {first for _, _, first in picks}
            if len(kinds) == 1 and len(picks) == 1:
                name_, proto_, kind = picks[0]
                self.set('VoicePathName', name_)
                self.set('VoicePathDesc', proto_)
                self.cls_key('VoiceKindCls', kind)
                self.member('VoiceKindFieldName', 'VoiceKindFieldDesc', kind, 'f', kind, static=True)

    def room(self):
        ids = {'RoomKickCls': '/cgi-bin/micromsg-bin/delchatroommember',
               'RoomAdminAddCls': '/cgi-bin/micromsg-bin/addchatroomadmin',
               'RoomAdminDelCls': '/cgi-bin/micromsg-bin/delchatroomadmin'}
        scene = None
        for key, text in ids.items():
            found = list(self.anchor(text))
            if len(found) == 1:
                self.cls_key(key, found[0])
                if key == 'RoomAdminAddCls':
                    scene = found[0]
#The admin scenes are cgi wrappers : their base class holds the request object(kick is a plain scene).
        cgi = self.anchor('MicroMsg.Cgi')
        if scene:
            base = [c for c in self.supers(scene)[1:] if c in cgi and any(not f & STATIC for _, _, f in self.fields(c))]
            if base:
                self.cls_key('RoomCgiBaseCls', base[0])
        if 'RoomCgiBaseCls' in self.r:
            base = descriptor(self.r['RoomCgiBaseCls'])
            self.member('RoomCgiReqName', 'RoomCgiReqDesc', base, 'f', 'L*;', static=False)
        runners = self.anchor('MicroMsg.RunCgi')
        for c in runners:
            found = [(n, p) for n, p, f in self.methods(c, pattern='(L*;L*;Z)L*;', static=True)]
            if len(found) == 1:
                self.cls_key('RoomRunnerCls', c)
                self.set('RoomRunnerName', found[0][0])
                self.set('RoomRunnerDesc', found[0][1])

    def pat(self):
        for c in self.anchor('/cgi-bin/micromsg-bin/sendpat'):
            if self.ctor(c, '(Landroid/util/Pair;Ljava/lang/String;Ljava/lang/String;I)V'):
                self.cls_key('PatSceneCls', c)
        shape_ = '(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IJ)Landroid/util/Pair;'
        found = [c for c in self.anchor('MicroMsg.PatMsgExtension') if any(True for _ in self.methods(c, pattern=shape_, static=False))]
        if len(found) == 1:
            self.cls_key('PatManagerCls', found[0])
            if self.service_of(found[0]):
                self.cls_key('PatServiceCls', self.service_of(found[0]))
            self.member('PatInsertName', None, found[0], 'm', shape_, static=False)

#Keys no rule derives : they are not obfuscated(a field of WeChat's message base class) or are a
#behaviour switch that the new build inherits from the one before it(see ThreadLooper in wx_send.cpp).
CARRIED = ('ThreadLooper', 'MsgInfoIsSendField')


def derive(apk, hints='8.0.79'):
    sets, order = table()
    index = wxindex.load(apk)
    finder = Finder(index, sets[hints] if hints else {})
    found = finder.run()
    for key in CARRIED:
        found.setdefault(key, 'field_isSend' if key == 'MsgInfoIsSendField' else sets[hints][key])
    return found, finder.why, sets, order


def main():
    apk = sys.argv[1]
    hints = ([a.split('=')[1] for a in sys.argv if a.startswith('--hints=')] or ['8.0.79'])[0]
    found, why, sets, order = derive(apk, hints or None)
    for key in order:
        mark = ''
        if '--check' in sys.argv:
            same = [label for label, values in sets.items() if values[key] == found.get(key)]
            mark = '   == ' + ','.join(same) if same else ('   (no match)' if key in found else '   MISSING')
        print('%-26s %s%s' % (key, found.get(key, '-'), mark))
    for key, reason in why.items():
        print('! %s: %s' % (key, reason))


if __name__ == '__main__':
    main()
