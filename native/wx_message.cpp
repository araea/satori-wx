#include "wx_message.h"
#include "media.h"
#include "textbuf.h"
#include "xml_lite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace satori {
namespace {
// WeChat inserts this after "@name" when a mention is made from the picker.
constexpr char kMentionEnd[] = "\xE2\x80\x85";  // U+2005 FOUR-PER-EM SPACE
constexpr size_t kMentionMax = 96;

bool RevokeType(int type) { return type == 268445456 || type == 285222674 || (type & 0xFFFF) == 10002; }
bool SystemType(int type) { return (type & 0xFFFF) == 10000 || RevokeType(type); }
bool AppMsgType(int type) { return type == 49 || (type & 0xFFFF) == 49; }

bool AccountChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '@' || c == '.';
}

// Splits the "<sender>:" header WeChat prepends to group messages (and, for stickers and videos,
// to every message). Returns the body.
const char *SplitHeader(const MessageRow &row, bool group, char *sender, size_t capacity) {
    sender[0] = 0;
    const char *content = row.content ? row.content : "";
    const bool always = row.type == 47 || row.type == 43;
    if (!always && !(group && !row.is_send)) return content;
    size_t n = 0;
    while (content[n] && AccountChar(content[n]) && n < capacity - 1) ++n;
    if (!n || content[n] != ':') return content;
    memcpy(sender, content, n);
    sender[n] = 0;
    if (always) {
        // "sender:0:1:<md5>:sender*#*\n<xml>" for stickers, "sender:<seconds>:<flag>" for video.
        const char *newline = strchr(content + n, '\n');
        return newline ? newline + 1 : content + strlen(content);
    }
    const char *body = content + n + 1;
    if (*body == '\n' || *body == ' ') ++body;  // voice messages use ": ", everything else ":\n"
    return body;
}

// Appends `text` as Satori text, turning WeChat's "@name<U+2005>" mentions into <at> elements
// in the order the mention list gives them. Ids left over (the sender edited the text after
// picking) are put in front so a mention is never silently lost.
void AppendTextWithMentions(TextBuf &out, const char *text, const char *at_list) {
    char ids[16][96];
    int count = 0;
    if (at_list) {
        const char *p = at_list;
        while (*p && count < 16) {
            const char *comma = strchr(p, ',');
            const size_t n = comma ? static_cast<size_t>(comma - p) : strlen(p);
            if (n && n < sizeof(ids[0])) { memcpy(ids[count], p, n); ids[count][n] = 0; ++count; }
            p = comma ? comma + 1 : p + n;
        }
    }
    TextBuf body;
    int used = 0;
    const char *p = text;
    while (*p) {
        if (*p == '@' && used < count) {
            const char *name = p + 1;
            const char *end = strstr(name, kMentionEnd);
            const bool plausible = end && static_cast<size_t>(end - name) <= kMentionMax && end > name &&
                                   !memchr(name, '\n', static_cast<size_t>(end - name)) && !memchr(name, '@', static_cast<size_t>(end - name));
            if (plausible) {
                if (!strcmp(ids[used], "notify@all")) body.Append("<at type=\"all\"/>");
                else {
                    body.Append("<at id=\""); body.Attr(ids[used]); body.Append("\" name=\"");
                    body.Attr(name, static_cast<size_t>(end - name)); body.Append("\"/>");
                }
                ++used;
                p = end + sizeof(kMentionEnd) - 1;
                // The separator WeChat inserted reads as the space after the mention; do not
                // double it when the text already has one.
                if (*p != ' ') body.Append(' ');
                continue;
            }
        }
        // Copy one UTF-8 character as text.
        size_t n = 1;
        const unsigned char lead = static_cast<unsigned char>(*p);
        if (lead >= 0xF0) n = 4; else if (lead >= 0xE0) n = 3; else if (lead >= 0xC0) n = 2;
        size_t have = 0;
        while (have < n && p[have]) ++have;
        body.Text(p, have);
        p += have;
    }
    for (int i = used; i < count; ++i) {
        if (!strcmp(ids[i], "notify@all")) out.Append("<at type=\"all\"/> ");
        else { out.Append("<at id=\""); out.Attr(ids[i]); out.Append("\"/> "); }
    }
    if (body.data) out.Append(body.data, body.size);
}

// "%lld ms" as seconds with up to three decimals: 4746 -> "4.746", 5000 -> "5".
void Seconds(long long milliseconds, char *out, size_t capacity) {
    if (milliseconds < 0) milliseconds = 0;
    const long long whole = milliseconds / 1000, frac = milliseconds % 1000;
    if (!frac) { snprintf(out, capacity, "%lld", whole); return; }
    char digits[8];
    snprintf(digits, sizeof(digits), "%03lld", frac);
    for (int i = 2; i >= 0 && digits[i] == '0'; --i) digits[i] = 0;
    snprintf(out, capacity, "%lld.%s", whole, digits);
}

// The message id as text, for a link.
bool Link(const char *self_id, const char *kind, long long id, TextBuf &out) {
    if (!self_id || !*self_id || id <= 0) return false;
    char text[24], link[256];
    snprintf(text, sizeof(text), "%lld", id);
    if (!MediaLink(self_id, kind, text, link, sizeof(link))) return false;
    out.Attr(link);
    return true;
}

// A media element whose source is one of our signed links, or a text placeholder when there is
// no login to bind the link to.
void MediaElement(TextBuf &out, const char *tag, const char *kind, const MessageRow &row, const char *self_id,
                  const char *placeholder, const char *extra_attrs = nullptr, const char *poster_kind = nullptr) {
    TextBuf link;
    if (!Link(self_id, kind, row.id, link) || !link.data) { out.Text(placeholder); return; }
    out.Append("<"); out.Append(tag); out.Append(" src=\""); out.Append(link.data, link.size); out.Append("\"");
    if (poster_kind) {
        TextBuf poster;
        if (Link(self_id, poster_kind, row.id, poster) && poster.data) { out.Append(" poster=\""); out.Append(poster.data, poster.size); out.Append("\""); }
    }
    if (extra_attrs && *extra_attrs) { out.Append(' '); out.Append(extra_attrs); }
    out.Append("/>");
}

// WeChat stores URLs inside sticker attributes with ':' rewritten to "*#*".
void UnmaskUrl(const char *in, char *out, size_t capacity) {
    size_t used = 0;
    while (*in && used + 1 < capacity) {
        if (!strncmp(in, "*#*", 3)) { out[used++] = ':'; in += 3; }
        else out[used++] = *in++;
    }
    out[used] = 0;
}

bool SafeUrl(const char *url) { return !strncasecmp(url, "http://", 7) || !strncasecmp(url, "https://", 8); }

// A reply: the appmsg's own title is the new text; <refermsg> describes the original.
void DecodeQuote(XmlSlice appmsg, const MessageRow &, Decoded *out) {
    char title[8192];
    XmlGetText(appmsg, "title", title, sizeof(title));
    TextBuf body;
    body.Text(title);
    out->content = body.Take();
    char type[16] = {}, from[96] = {}, chat[96] = {};
    XmlGetText(appmsg, "refermsg/svrid", out->refer_svr_id, sizeof(out->refer_svr_id));
    XmlGetText(appmsg, "refermsg/fromusr", from, sizeof(from));
    XmlGetText(appmsg, "refermsg/chatusr", chat, sizeof(chat));
    XmlGetText(appmsg, "refermsg/displayname", out->refer_name, sizeof(out->refer_name));
    XmlGetText(appmsg, "refermsg/type", type, sizeof(type));
    snprintf(out->refer_user, sizeof(out->refer_user), "%s", *chat ? chat : from);
    char *original = static_cast<char *>(malloc(4096));
    if (!original) return;
    XmlGetText(appmsg, "refermsg/content", original, 4096);
    const int refer_type = atoi(type);
    // Only a plain text original quotes as itself; anything else quotes as what it was.
    if (refer_type == 1 || (!refer_type && *original && *original != '<')) {
        // A quoted group message still carries its "wxid:\n" header.
        char *newline = strstr(original, ":\n");
        if (newline && newline - original < 90) {
            bool header = true;
            for (const char *c = original; c < newline; ++c) if (!AccountChar(*c)) { header = false; break; }
            if (header) memmove(original, newline + 2, strlen(newline + 2) + 1);
        }
        out->refer_text = original;
        return;
    }
    const char *label = refer_type == 3 ? "[图片]" : refer_type == 34 ? "[语音]" : refer_type == 43 ? "[视频]" :
                        refer_type == 47 ? "[动画表情]" : refer_type == 48 ? "[位置]" : refer_type == 42 ? "[名片]" : nullptr;
    if (!label && *original == '<') {
        XmlSlice inner = XmlDocument(original);
        char quoted_title[256];
        if (XmlGetText(inner, "msg/appmsg/title", quoted_title, sizeof(quoted_title)) && *quoted_title) {
            snprintf(original, 4096, "[%s]", quoted_title);
            out->refer_text = original;
            return;
        }
    }
    snprintf(original, 4096, "%s", label ? label : "[消息]");
    out->refer_text = original;
}

// Renders one "拍了拍" record's template: each "${wxid}" placeholder becomes the id, or "你"
// when it is this account. The template is plain text apart from the placeholders.
void ExpandPatTemplate(const char *tpl, const char *self_id, TextBuf &text) {
    for (const char *p = tpl; *p;) {
        if (p[0] == '$' && p[1] == '{') {
            const char *close = strchr(p + 2, '}');
            const size_t n = close ? static_cast<size_t>(close - (p + 2)) : 0;
            if (close && n < 96) {
                char id[96];
                memcpy(id, p + 2, n); id[n] = 0;
                const bool mine = self_id && *self_id && !strcmp(id, self_id);
                text.Text(mine ? "你" : id);
                p = close + 1;
                continue;
            }
        }
        text.Text(p, 1);
        ++p;
    }
}

// A "拍了拍" row (appmsg type 62): <patMsg><records><record> carries who patted whom, plus the
// server's own tip template ("${a}" 拍了拍 "${b}"). One row may aggregate several records. It
// is delivered as a message whose author is the first record's pat-er, so a client sees who
// patted (and can react to being patted); a wrapper without readable records stays bookkeeping.
void DecodePat(XmlSlice appmsg, const char *self_id, Decoded *out) {
    XmlSlice records;
    if (!XmlPath(appmsg, "patMsg/records", &records)) { out->kind = MsgKind::System; return; }
    TextBuf text;
    char first_sender[96] = {};
    int delivered = 0;
    for (int i = 0; i < 32; ++i) {
        XmlSlice record;
        if (!XmlChildAt(records, "record", i, &record)) break;
        char from[96] = {}, patted[96] = {}, tpl[512] = {};
        XmlGetText(record, "fromUser", from, sizeof(from));
        XmlGetText(record, "pattedUser", patted, sizeof(patted));
        if (!*from && !*patted) continue;
        XmlGetText(record, "template", tpl, sizeof(tpl));
        if (delivered) text.Append('\n');
        if (*tpl) {
            ExpandPatTemplate(tpl, self_id, text);
        } else {
            text.Append('"'); text.Text(from); text.Append('"');
            text.Text(" \xE6\x8B\x8D\xE4\xBA\x86\xE6\x8B\x8D ");
            text.Append('"'); text.Text(patted); text.Append('"');
        }
        if (!delivered) snprintf(first_sender, sizeof(first_sender), "%s", from);
        ++delivered;
    }
    if (!delivered || text.failed) { out->kind = MsgKind::System; return; }
    out->kind = MsgKind::Other;
    out->deliver = true;
    out->content = text.Take();
    snprintf(out->sender, sizeof(out->sender), "%s", first_sender);
}

// A merged-forward card ("聊天记录", appmsg 19) carries its lines in <recorditem>: a <recordinfo> whose
// <datalist> holds one <dataitem> per line. Satori writes that as a `forward` message holding one
// `<message>` per line, each with its <author>. A line that is not text (WeChat keeps its media on
// the CDN, which this module cannot fetch) reads as its "[图片]"-style label; a record inside a record
// nests once more.
constexpr size_t kRecordMax = 96 * 1024;   // an event has to fit the bus
constexpr int kRecordDepth = 2;

const char *ItemLabel(int datatype) {
    switch (datatype) {
        case 2: return "[图片]";
        case 3: return "[语音]";
        case 4: return "[视频]";
        case 5: return "[链接]";
        case 6: return "[位置]";
        case 7: return "[音乐]";
        case 8: return "[文件]";
        case 17: return "[聊天记录]";
        case 19: return "[小程序]";
        default: return "[消息]";
    }
}

void AppendRecord(TextBuf &out, XmlSlice info, int depth) {
    char title[256] = {};
    XmlGetText(info, "title", title, sizeof(title));
    out.Append("<message forward");
    if (*title) { out.Append(" title=\""); out.Attr(title); out.Append("\""); }
    out.Append(">");
    XmlSlice list;
    if (XmlPath(info, "datalist", &list)) {
        for (int i = 0; i < 200 && out.size < kRecordMax; ++i) {
            XmlSlice item, attrs;
            if (!XmlChildAt(list, "dataitem", i, &item, &attrs)) break;
            char type_text[16] = {}, name[256] = {}, avatar[512] = {}, text[8192] = {};
            XmlAttribute(attrs, "datatype", type_text, sizeof(type_text));
            const int datatype = atoi(type_text);
            XmlGetText(item, "sourcename", name, sizeof(name));
            XmlGetText(item, "sourceheadurl", avatar, sizeof(avatar));
            out.Append("<message>");
            if (*name || SafeUrl(avatar)) {
                out.Append("<author");
                if (*name) { out.Append(" name=\""); out.Attr(name); out.Append("\""); }
                if (SafeUrl(avatar)) { out.Append(" avatar=\""); out.Attr(avatar); out.Append("\""); }
                out.Append("/>");
            }
            XmlSlice nested;
            if (datatype == 1) {
                XmlGetText(item, "datadesc", text, sizeof(text));
                out.Text(text);
            } else if (datatype == 17 && depth < kRecordDepth && XmlPath(item, "recordxml/recordinfo", &nested)) {
                AppendRecord(out, nested, depth + 1);
            } else {
                // The label, then whatever name the line has (a file's name, a link's title).
                out.Text(ItemLabel(datatype));
                if (datatype == 2 || datatype == 3 || datatype == 4) { /* nothing more to say */ }
                else if (XmlGetText(item, "datatitle", text, sizeof(text)) && *text) { out.Append(' '); out.Text(text); }
            }
            out.Append("</message>");
        }
    }
    out.Append("</message>");
}

bool DecodeRecord(XmlSlice appmsg, Decoded *out) {
    XmlSlice raw;
    if (!XmlPath(appmsg, "recorditem", &raw) || !raw.Size()) return false;
    char *info = static_cast<char *>(malloc(raw.Size() + 1));
    if (!info) return false;
    XmlText(raw, info, raw.Size() + 1);
    XmlSlice record;
    TextBuf text;
    const bool ok = XmlPath(XmlDocument(info), "recordinfo", &record) && XmlPath(record, "datalist", &record);
    if (ok) {
        XmlPath(XmlDocument(info), "recordinfo", &record);
        AppendRecord(text, record, 0);
        out->kind = MsgKind::Other;
        out->deliver = true;
        out->content = text.Take();
    }
    free(info);
    return ok && out->content;
}

void DecodeAppMsg(const MessageRow &row, const char *body, const char *self_id, Decoded *out) {
    out->deliver = false;  // each branch below opts in once it has something to say
    XmlSlice root = XmlDocument(body);
    XmlSlice appmsg;
    if (!XmlPath(root, "msg/appmsg", &appmsg)) {
        // WeChat sometimes hands over a bare <appmsg>.
        if (!XmlPath(root, "appmsg", &appmsg)) {
            TextBuf text; text.Text("[消息]");
            out->kind = MsgKind::Other; out->deliver = true; out->content = text.Take();
            return;
        }
    }
    char type_text[16] = {};
    XmlGetText(appmsg, "type", type_text, sizeof(type_text));
    const int subtype = atoi(type_text);
    if (subtype == 57) { out->kind = MsgKind::Quote; out->deliver = true; DecodeQuote(appmsg, row, out); return; }
    if (subtype == 62) { DecodePat(appmsg, self_id, out); return; }  // "拍了拍": who patted whom
    if (subtype == 19 && DecodeRecord(appmsg, out)) return;            // 聊天记录: a merged forward

    char title[1024] = {}, description[2048] = {}, url[2048] = {};
    XmlGetText(appmsg, "title", title, sizeof(title));
    XmlGetText(appmsg, "des", description, sizeof(description));
    XmlGetText(appmsg, "url", url, sizeof(url));
    TextBuf text;
    out->deliver = true;
    if (subtype == 6) {
        out->kind = MsgKind::File;
        TextBuf link;
        if (Link(self_id, "file", row.id, link) && link.data) {
            text.Append("<file src=\""); text.Append(link.data, link.size); text.Append("\"");
            if (*title) { text.Append(" title=\""); text.Attr(title); text.Append("\""); }
            text.Append("/>");
        } else {
            text.Text("[文件] "); text.Text(title);
        }
    } else if (SafeUrl(url) && subtype != 2000 && subtype != 2001) {
        out->kind = MsgKind::Link;
        text.Append("<a href=\""); text.Attr(url); text.Append("\">");
        text.Text(*title ? title : url); text.Append("</a>");
        if (*description && strcmp(description, title)) { text.Append('\n'); text.Text(description); }
    } else {
        out->kind = MsgKind::Other;
        text.Append('['); text.Text(*title ? title : "消息"); text.Append(']');
        if (*description && strcmp(description, title)) { text.Append(' '); text.Text(description); }
    }
    out->content = text.Take();
}
} // namespace

Decoded::~Decoded() { Reset(); }

void Decoded::Reset() {
    free(content); free(refer_text);
    content = refer_text = nullptr;
    kind = MsgKind::Ignored;
    deliver = false;
    sender[0] = refer_svr_id[0] = refer_user[0] = refer_name[0] = 0;
}

bool IsRevokeType(int type) { return RevokeType(type); }
bool IsSystemType(int type) { return SystemType(type); }

void DecodeMessage(const MessageRow &row, const char *self_id, Decoded *out) {
    out->Reset();
    const char *talker = row.talker ? row.talker : "";
    const bool group = strstr(talker, "@chatroom") != nullptr;
    if (RevokeType(row.type)) { out->kind = MsgKind::Revoke; return; }
    if (SystemType(row.type)) { out->kind = MsgKind::System; return; }
    const int base = row.type;
    if (base != 1 && base != 3 && base != 34 && base != 43 && base != 47 && base != 48 && base != 42 && !AppMsgType(base)) return;

    char sender[96];
    const char *body = SplitHeader(row, group, sender, sizeof(sender));
    snprintf(out->sender, sizeof(out->sender), "%s", sender);
    // WeChat's own service rows share the appmsg type but carry a <sysmsg>.
    const char *probe = body;
    while (*probe == ' ' || *probe == '\n' || *probe == '\r' || *probe == '\t') ++probe;
    if (!strncmp(probe, "<sysmsg", 7)) { out->kind = MsgKind::System; return; }

    TextBuf text;
    out->deliver = true;
    if (base == 1) {
        out->kind = MsgKind::Text;
        // The mention list lives in <msgsource>, not in the text, and only groups have mentions.
        char at_list[1536] = {};
        if (group && row.msg_source && *row.msg_source)
            XmlGetText(XmlDocument(row.msg_source), "msgsource/atuserlist", at_list, sizeof(at_list));
        AppendTextWithMentions(text, body, at_list);
        out->content = text.Take();
        if (!out->content) out->deliver = false;
        return;
    }
    XmlSlice root = XmlDocument(body);
    if (base == 3) {
        out->kind = MsgKind::Image;
        MediaElement(text, "img", "image", row, self_id, "[图片]");
    } else if (base == 34) {
        out->kind = MsgKind::Voice;
        char length[16] = {};
        XmlGetAttribute(root, "msg/voicemsg", "voicelength", length, sizeof(length));
        if (!*length) {
            // A voice we sent ourselves is stored as "<wxid>:<milliseconds>:<flag>" until WeChat rewrites it.
            const char *own = row.content ? row.content : "";
            const char *colon = *own == '<' ? nullptr : strchr(own, ':');
            char *end = nullptr;
            const long ms = colon ? strtol(colon + 1, &end, 10) : 0;
            if (colon && end != colon + 1 && *end == ':' && ms > 0) snprintf(length, sizeof(length), "%ld", ms);
        }
        char attrs[48] = {}, seconds[24];
        if (*length) { Seconds(atoll(length), seconds, sizeof(seconds)); snprintf(attrs, sizeof(attrs), "duration=\"%s\"", seconds); }
        MediaElement(text, "audio", "voice", row, self_id, "[语音]", attrs);
    } else if (base == 43) {
        out->kind = MsgKind::Video;
        // Either "sender:<seconds>:<flag>" (already split off above) or a <videomsg> XML.
        char attrs[48] = {};
        char length[16] = {};
        if (XmlGetAttribute(root, "msg/videomsg", "playlength", length, sizeof(length)) && *length)
            snprintf(attrs, sizeof(attrs), "duration=\"%lld\"", atoll(length));
        else {
            const char *content = row.content ? row.content : "";
            const char *first = *content == '<' ? nullptr : strchr(content, ':');
            if (first && first[1] >= '0' && first[1] <= '9') snprintf(attrs, sizeof(attrs), "duration=\"%lld\"", atoll(first + 1));
        }
        MediaElement(text, "video", "video", row, self_id, "[视频]", attrs, "videothumb");
    } else if (base == 47) {
        out->kind = MsgKind::Emoji;
        char masked[1536] = {}, url[1536];
        XmlGetAttribute(root, "msg/emoji", "cdnurl", masked, sizeof(masked));
        UnmaskUrl(masked, url, sizeof(url));
        if (SafeUrl(url)) { text.Append("<img src=\""); text.Attr(url); text.Append("\"/>"); }
        else MediaElement(text, "img", "emoji", row, self_id, "[动画表情]");
    } else if (base == 48) {
        out->kind = MsgKind::Location;
        char name[256] = {}, label[512] = {}, latitude[32] = {}, longitude[32] = {};
        XmlGetAttribute(root, "msg/location", "poiname", name, sizeof(name));
        XmlGetAttribute(root, "msg/location", "label", label, sizeof(label));
        XmlGetAttribute(root, "msg/location", "x", latitude, sizeof(latitude));
        XmlGetAttribute(root, "msg/location", "y", longitude, sizeof(longitude));
        text.Text("[位置]");
        if (*name) { text.Append(' '); text.Text(name); }
        if (*label) { text.Text(*name ? "（" : " "); text.Text(label); if (*name) text.Text("）"); }
        if (*latitude && *longitude) { text.Append(' '); text.Text(latitude); text.Append(','); text.Text(longitude); }
    } else if (base == 42) {
        out->kind = MsgKind::Card;
        char nickname[256] = {}, username[128] = {};
        XmlGetAttribute(root, "msg", "nickname", nickname, sizeof(nickname));
        XmlGetAttribute(root, "msg", "username", username, sizeof(username));
        text.Text("[名片]");
        if (*nickname) { text.Append(' '); text.Text(nickname); }
        if (*username) { text.Text(" ("); text.Text(username); text.Append(')'); }
    } else {
        DecodeAppMsg(row, body, self_id, out);
        return;
    }
    out->content = text.Take();
    if (!out->content) out->deliver = false;
}
} // namespace satori
