#include "wx_forward.h"
#include "media.h"
#include "protocol.h"
#include "textbuf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

namespace satori {
namespace {
void Fail(ForwardError *error, const char *code, const char *format, int number = 0) {
    if (!error) return;
    snprintf(error->code, sizeof(error->code), "%s", code);
    snprintf(error->detail, sizeof(error->detail), format, number);
}

bool HttpUrl(const char *text) { return !strncmp(text, "http://", 7) || !strncmp(text, "https://", 8); }

void Trim(char *text) {
    char *end = text + strlen(text);
    while (end > text && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) --end;
    *end = 0;
    const char *start = text;
    while (*start == '\n' || *start == '\r' || *start == ' ' || *start == '\t') ++start;
    if (start != text) memmove(text, start, strlen(start) + 1);
}

// Copies `from[0, size)` into `out` without the stretch [cut_begin, cut_end).
void CopyWithout(const char *from, size_t size, size_t cut_begin, size_t cut_end, char *out) {
    memcpy(out, from, cut_begin);
    memcpy(out + cut_begin, from + cut_end, size - cut_end);
    out[cut_begin + size - cut_end] = 0;
}

// The <author> of one message: its attributes, or its text as the name. Returns the content with the
// element taken out (malloc'd), null on allocation failure.
char *TakeAuthor(const char *inner, ForwardEntry *entry) {
    const size_t size = strlen(inner);
    char *rest = static_cast<char *>(malloc(size + 1));
    if (!rest) return nullptr;
    memcpy(rest, inner, size + 1);
    ImageSpan tag;
    if (!FirstTag(inner, "author", &tag)) return rest;
    TagAttribute(inner, tag, "id", entry->author_id, sizeof(entry->author_id));
    TagAttribute(inner, tag, "name", entry->author_name, sizeof(entry->author_name));
    char avatar[sizeof(entry->author_avatar)] = {};
    if (TagAttribute(inner, tag, "avatar", avatar, sizeof(avatar)) && HttpUrl(avatar)) memcpy(entry->author_avatar, avatar, strlen(avatar) + 1);
    size_t cut_end = tag.end;
    if (tag.end >= tag.begin + 2 && inner[tag.end - 2] != '/') {
        // <author>Name</author>: the element's text names the author when the attribute does not.
        const char *close = nullptr;
        for (const char *p = inner + tag.end; *p; ++p)
            if (!strncasecmp(p, "</author", 8)) { close = p; break; }
        if (close) {
            const char *after = strchr(close, '>');
            if (!entry->author_name[0]) {
                char *label = static_cast<char *>(malloc(static_cast<size_t>(close - (inner + tag.end)) + 1));
                if (label) {
                    memcpy(label, inner + tag.end, static_cast<size_t>(close - (inner + tag.end)));
                    label[close - (inner + tag.end)] = 0;
                    PlainText(label, entry->author_name, sizeof(entry->author_name));
                    Trim(entry->author_name);
                    free(label);
                }
            }
            cut_end = after ? static_cast<size_t>(after + 1 - inner) : size;
        }
    }
    CopyWithout(inner, size, tag.begin, cut_end, rest);
    return rest;
}

// ---- writing the record -------------------------------------------------------------------------
// Text inside <recordinfo>: the five markup characters, line breaks as character references (as
// WeChat writes them), and no control characters XML cannot carry.
void Xml(TextBuf &out, const char *text) {
    for (; *text; ++text) {
        const unsigned char c = static_cast<unsigned char>(*text);
        switch (c) {
            case '&': out.Append("&amp;"); break;
            case '<': out.Append("&lt;"); break;
            case '>': out.Append("&gt;"); break;
            case '"': out.Append("&quot;"); break;
            case '\'': out.Append("&apos;"); break;
            case '\n': out.Append("&#x0A;"); break;
            case '\r': break;
            case '\t': out.Append('\t'); break;
            default: if (c >= 0x20) out.Append(*text); break;
        }
    }
}

// The first `limit` code points of `text`, with its line breaks turned into spaces; "…" when cut.
size_t Preview(const char *text, size_t limit, char *out, size_t capacity) {
    size_t used = 0, points = 0;
    bool cut = false;
    for (const char *p = text; *p;) {
        const unsigned char c = static_cast<unsigned char>(*p);
        const size_t length = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (points == limit) { cut = true; break; }
        if (used + length + 4 >= capacity) { cut = true; break; }
        if (c == '\n' || c == '\r') {
            if (used && out[used - 1] != ' ') out[used++] = ' ';
        } else {
            for (size_t i = 0; i < length && p[i]; ++i) out[used++] = p[i];
        }
        p += length;
        ++points;
    }
    while (used && out[used - 1] == ' ') --used;
    if (cut) { memcpy(out + used, "\xE2\x80\xA6", 3); used += 3; }
    out[used] = 0;
    return used;
}

void Clock(long long seconds, char *out, size_t capacity) {
    time_t t = static_cast<time_t>(seconds);
    struct tm parts{};
    localtime_r(&t, &parts);
    strftime(out, capacity, "%Y-%m-%d %H:%M:%S", &parts);
}

const char *Name(const ForwardEntry &entry) {
    return entry.author_name[0] ? entry.author_name : entry.author_id[0] ? entry.author_id : "微信用户";
}
} // namespace

int ForwardParse(const char *content, size_t begin, size_t end, ForwardEntry *out, size_t max,
                 char *title, size_t title_capacity, ForwardError *error) {
    if (title && title_capacity) title[0] = 0;
    if (!content || !out || !max) return -1;
    if (title && title_capacity) {
        ImageSpan open;
        if (FirstTag(content + begin, "message", &open)) {
            char value[200] = {};
            if (TagAttribute(content + begin, open, "title", value, sizeof(value))) {
                Trim(value);
                snprintf(title, title_capacity, "%s", value);
            }
        }
    }
    auto *children = static_cast<ForwardChild *>(malloc((max + 1) * sizeof(ForwardChild)));
    if (!children) { Fail(error, "internal_error", "out of memory"); return -1; }
    const size_t found = ForwardChildren(content, begin, end, children, max + 1);
    if (!found) { free(children); Fail(error, "forward_empty", "a <message forward> needs <message> elements inside it"); return -1; }
    if (found > max) { free(children); Fail(error, "forward_too_many", "at most %d messages fit in one merged forward", static_cast<int>(max)); return -1; }
    int count = 0;
    for (size_t i = 0; i < found; ++i) {
        ForwardEntry &entry = out[count];
        entry = {};
        const ForwardChild &child = children[i];
        // `id` on the child's own tag: a message of this conversation to embed, when nothing else is written in it.
        ImageSpan open{child.begin, child.inner_begin};
        char id[sizeof(entry.ref_id) + 8] = {};
        const bool has_id = TagAttribute(content, open, "id", id, sizeof(id)) && *id;
        const size_t inner_size = child.inner_end - child.inner_begin;
        char *inner = static_cast<char *>(malloc(inner_size + 1));
        if (!inner) { free(children); Fail(error, "internal_error", "out of memory"); return -1; }
        memcpy(inner, content + child.inner_begin, inner_size);
        inner[inner_size] = 0;
        if (strcasestr(inner, "<message")) {
            free(inner); free(children);
            Fail(error, "forward_nested_unsupported", "a merged forward cannot hold another <message forward> or nested <message>s (message %d)", static_cast<int>(i + 1));
            return -1;
        }
        MediaSpan media[1];
        if (MediaSpans(inner, media, 1)) {
            free(inner); free(children);
            Fail(error, "forward_media_unsupported", "pictures, audio, video and files cannot be put in a merged forward yet (message %d)", static_cast<int>(i + 1));
            return -1;
        }
        char *rest = TakeAuthor(inner, &entry);
        free(inner);
        if (!rest) { free(children); Fail(error, "internal_error", "out of memory"); return -1; }
        // Wide enough that a text over the limit is noticed rather than silently cut.
        const size_t room = strlen(rest) + 8;
        char *flat = static_cast<char *>(malloc(room));
        if (!flat) { free(rest); free(children); Fail(error, "internal_error", "out of memory"); return -1; }
        OutgoingMention mentions[16];
        size_t mention_count = 0;
        OutgoingText(rest, flat, room, mentions, 16, &mention_count, nullptr, nullptr);
        free(rest);
        Trim(flat);
        const size_t length = strlen(flat);
        if (length > kForwardText) {
            free(flat); free(children);
            Fail(error, "content_too_long", "message %d is longer than 4000 bytes", static_cast<int>(i + 1));
            return -1;
        }
        memcpy(entry.text, flat, length + 1);
        free(flat);
        if (!entry.text[0]) {
            if (!has_id) continue;   // nothing in it: not sent
            if (strlen(id) >= sizeof(entry.ref_id)) { free(children); Fail(error, "forward_message_not_found", "message %d: the id is not a message id", static_cast<int>(i + 1)); return -1; }
            memcpy(entry.ref_id, id, strlen(id) + 1);
        }
        ++count;
    }
    free(children);
    if (!count) { Fail(error, "forward_empty", "every message in the <message forward> is empty"); return -1; }
    return count;
}

bool ForwardBuild(const ForwardEntry *entries, size_t count, const char *title_override, bool group, long long now_s,
                  ForwardCard *card, ForwardError *error) {
    if (!entries || !count || !card) return false;
    *card = {};
    // The headline: what WeChat writes for a record of one, two or many speakers.
    if (title_override && *title_override) {
        snprintf(card->title, sizeof(card->title), "%s", title_override);
    } else {
        const char *first = nullptr, *second = nullptr;
        size_t distinct = 0;
        for (size_t i = 0; i < count && distinct < 3; ++i) {
            const char *name = Name(entries[i]);
            if (first && !strcmp(first, name)) continue;
            if (second && !strcmp(second, name)) continue;
            if (!first) first = name; else if (!second) second = name;
            ++distinct;
        }
        if (group || distinct >= 3) snprintf(card->title, sizeof(card->title), "群聊的聊天记录");
        else if (distinct == 2) snprintf(card->title, sizeof(card->title), "%s与%s的聊天记录", first, second);
        else snprintf(card->title, sizeof(card->title), "%s的聊天记录", first);
    }
    // The preview: four "name: text" lines, one line each, well inside what the card can show.
    size_t used = 0, points = 0;
    for (size_t i = 0; i < count && i < 4; ++i) {
        char line[256];
        char text[200];
        Preview(entries[i].text, 40, text, sizeof(text));
        const int written = snprintf(line, sizeof(line), "%s%s: %s", i ? "\n" : "", Name(entries[i]), text);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(line)) break;
        size_t line_points = 0;
        for (const char *p = line; *p; ++p) if ((static_cast<unsigned char>(*p) & 0xC0) != 0x80) ++line_points;
        if (points + line_points > 190 && i) break;
        if (used + static_cast<size_t>(written) + 1 >= sizeof(card->desc)) break;
        memcpy(card->desc + used, line, static_cast<size_t>(written) + 1);
        used += static_cast<size_t>(written);
        points += line_points;
    }

    TextBuf info;
    info.Append("<recordinfo><title>"); Xml(info, card->title); info.Append("</title><desc>"); Xml(info, card->desc);
    char number[32];
    snprintf(number, sizeof(number), "%zu", count);
    info.Append("</desc><datalist count=\""); info.Append(number); info.Append("\">");
    for (size_t i = 0; i < count; ++i) {
        const ForwardEntry &entry = entries[i];
        const long long when = entry.created_s > 0 ? entry.created_s : now_s;
        char clock[32], seed[64], id[33];
        Clock(when, clock, sizeof(clock));
        // An id WeChat keys the item by; unique within the card and stable for the same content.
        snprintf(seed, sizeof(seed), "%zu|%lld|%zu", i, when, strlen(entry.text));
        TextBuf key;
        key.Append(seed); key.Append(Name(entry)); key.Append(entry.text);
        Md5Hex(key.data ? key.data : "", key.size, id);
        info.Append("<dataitem datatype=\"1\" dataid=\""); info.Append(id); info.Append("\"><datadesc>"); Xml(info, entry.text);
        info.Append("</datadesc><sourcename>"); Xml(info, Name(entry)); info.Append("</sourcename>");
        if (entry.author_avatar[0]) { info.Append("<sourceheadurl>"); Xml(info, entry.author_avatar); info.Append("</sourceheadurl>"); }
        info.Append("<sourcetime>"); info.Append(clock); info.Append("</sourcetime><srcMsgCreateTime>");
        snprintf(number, sizeof(number), "%lld", when);
        info.Append(number); info.Append("</srcMsgCreateTime>");
        if (entry.svr_id > 0) { snprintf(number, sizeof(number), "%lld", entry.svr_id); info.Append("<fromnewmsgid>"); info.Append(number); info.Append("</fromnewmsgid>"); }
        info.Append("</dataitem>");
        if (info.size > kForwardRecordMax) { Fail(error, "forward_too_large", "the merged forward is larger than 256 KiB"); return false; }
    }
    info.Append("</datalist></recordinfo>");
    if (info.size > kForwardRecordMax) { Fail(error, "forward_too_large", "the merged forward is larger than 256 KiB"); return false; }
    card->record = info.Take();
    if (!card->record) { Fail(error, "internal_error", "out of memory"); return false; }
    return true;
}

char *ForwardContent(const ForwardEntry *entries, size_t count, const char *title_override) {
    TextBuf out;
    out.Append("<message forward");
    if (title_override && *title_override) { out.Append(" title=\""); out.Attr(title_override); out.Append('"'); }
    out.Append(">");
    for (size_t i = 0; i < count; ++i) {
        const ForwardEntry &entry = entries[i];
        out.Append("<message><author");
        if (entry.author_id[0]) { out.Append(" id=\""); out.Attr(entry.author_id); out.Append('"'); }
        out.Append(" name=\""); out.Attr(Name(entry)); out.Append('"');
        if (entry.author_avatar[0]) { out.Append(" avatar=\""); out.Attr(entry.author_avatar); out.Append('"'); }
        out.Append("/>"); out.Text(entry.text); out.Append("</message>");
    }
    out.Append("</message>");
    return out.Take();
}
} // namespace satori
