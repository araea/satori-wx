// Host-side tests for merge forwarding: reading a `<message forward>` container into entries, and
// writing the chat-record card (headline, preview, <recordinfo>) WeChat is asked to carry. Nothing
// here touches WeChat: the card is checked by reading it back with the same XML scanner the module
// uses on received records.
#include "protocol.h"
#include "wx_forward.h"
#include "xml_lite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
int failures = 0;

void Check(bool ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
void Eq(const char *got, const char *want, const char *what) {
    if (strcmp(got, want)) {
        fprintf(stderr, "FAIL: %s\n  got:  %s\n  want: %s\n", what, got, want);
        ++failures;
    }
}

struct Parsed {
    satori::ForwardEntry entries[satori::kForwardMax];
    int count;
    char title[200];
    satori::ForwardError error;
};

// Runs the parser over the first merge-forward part of `content`.
Parsed *Parse(const char *content) {
    auto *parsed = static_cast<Parsed *>(calloc(1, sizeof(Parsed)));
    satori::MessagePart parts[4];
    const size_t count = satori::MessageParts(content, parts, 4);
    size_t at = 0;
    while (at < count && parts[at].kind != 'm') ++at;
    if (at == count) {
        parsed->count = -2;
        return parsed;
    }
    parsed->count = satori::ForwardParse(content, parts[at].begin, parts[at].end, parsed->entries, satori::kForwardMax,
                                         parsed->title, sizeof(parsed->title), &parsed->error);
    return parsed;
}
} // namespace

int main() {
    // ---- reading the container --------------------------------------------------------------------------
    {
        Parsed *p = Parse("<message forward>"
                          "<message><author id=\"u1\" name=\"Alice\" avatar=\"https://example.com/a.png\"/>hi</message>"
                          "<message><author id=\"u2\" name=\"Bob\"/>yo &amp; co<br/>line2</message>"
                          "</message>");
        Check(p->count == 2, "two lines");
        if (p->count == 2) {
            Eq(p->entries[0].author_id, "u1", "author id");
            Eq(p->entries[0].author_name, "Alice", "author name");
            Eq(p->entries[0].author_avatar, "https://example.com/a.png", "author avatar");
            Eq(p->entries[0].text, "hi", "text");
            Eq(p->entries[1].text, "yo & co\nline2", "entities decoded, <br/> is a newline");
            Eq(p->entries[1].author_avatar, "", "no avatar given");
            Check(!p->entries[0].ref_id[0] && p->entries[0].created_s == 0, "nothing embedded");
        }
        Eq(p->title, "", "no title given");
        free(p);
    }
    {
        Parsed *p = Parse("<message forward title=\"周报\"><message><author name=\"A\"/>x</message></message>");
        Check(p->count == 1, "one line");
        Eq(p->title, "周报", "the title extension");
        free(p);
    }
    {
        Parsed *p =
            Parse("<message forward><message><author>Carol</author>text</message>"
                  "<message><author id=\"9\" avatar=\"internal:wechat/x/_tmp/a\"/>no avatar url</message></message>");
        Check(p->count == 2, "two lines");
        if (p->count == 2) {
            Eq(p->entries[0].author_name, "Carol", "the author element's text names the author");
            Eq(p->entries[0].text, "text", "the author element is not part of the text");
            Eq(p->entries[1].author_avatar, "", "an avatar that is not http(s) is dropped");
        }
        free(p);
    }
    {
        Parsed *p = Parse(
            "<message forward><message id=\"12\"/><message id=\"13\">own words</message><message>  </message><message>\n</message></message>");
        Check(p->count == 2, "blank messages are skipped");
        if (p->count == 2) {
            Eq(p->entries[0].ref_id, "12", "an id alone embeds that message");
            Eq(p->entries[0].text, "", "...its text is filled in later");
            Eq(p->entries[1].ref_id, "", "written text wins over an id");
            Eq(p->entries[1].text, "own words", "text");
        }
        free(p);
    }
    {
        Parsed *p = Parse(
            "<message forward><message><at id=\"7\" name=\"Dan\"/> see <a href=\"https://e.com\">this</a></message></message>");
        Check(p->count == 1, "one line");
        if (p->count == 1)
            Eq(p->entries[0].text, "@Dan\xE2\x80\x85 see this (https://e.com)",
               "mentions and links read as they do in a text message");
        free(p);
    }
    {
        Parsed *p = Parse("<message forward> </message>");
        Check(p->count == -1 && !strcmp(p->error.code, "forward_empty"), "no messages inside");
        free(p);
        p = Parse("<message forward><message> </message></message>");
        Check(p->count == -1 && !strcmp(p->error.code, "forward_empty"), "only empty messages");
        free(p);
        p = Parse("<message forward><message>a<message>b</message></message></message>");
        Check(p->count == -1 && !strcmp(p->error.code, "forward_nested_unsupported"), "a nested <message> is refused");
        free(p);
        p = Parse("<message forward><message><img src=\"x\"/></message></message>");
        Check(p->count == -1 && !strcmp(p->error.code, "forward_media_unsupported"), "media in a line is refused");
        free(p);
        p = Parse("<message forward><message id=\"123456789012345678901234567890\"/></message>");
        Check(p->count == -1 && !strcmp(p->error.code, "forward_message_not_found"), "an id too long to be one");
        free(p);
    }
    {
        // Bounds: 101 lines, and one line over 4000 bytes.
        char *many = static_cast<char *>(malloc(64 * 1024));
        strcpy(many, "<message forward>");
        for (int i = 0; i < 101; ++i) strcat(many, "<message>x</message>");
        strcat(many, "</message>");
        Parsed *p = Parse(many);
        Check(p->count == -1 && !strcmp(p->error.code, "forward_too_many"), "101 lines are too many");
        free(p);
        strcpy(many, "<message forward><message>");
        for (int i = 0; i < 4001; ++i) strcat(many, "a");
        strcat(many, "</message></message>");
        p = Parse(many);
        Check(p->count == -1 && !strcmp(p->error.code, "content_too_long"), "a line over 4000 bytes");
        free(p);
        strcpy(many, "<message forward><message>");
        for (int i = 0; i < 4000; ++i) strcat(many, "a");
        strcat(many, "</message></message>");
        p = Parse(many);
        Check(p->count == 1, "4000 bytes is fine");
        free(p);
        free(many);
    }

    // ---- writing the card -------------------------------------------------------------------------------
    auto *entries = static_cast<satori::ForwardEntry *>(calloc(satori::kForwardMax, sizeof(satori::ForwardEntry)));
    auto Fill = [&](int i, const char *id, const char *name, const char *text) {
        entries[i] = {};
        snprintf(entries[i].author_id, sizeof(entries[i].author_id), "%s", id);
        snprintf(entries[i].author_name, sizeof(entries[i].author_name), "%s", name);
        snprintf(entries[i].text, sizeof(entries[i].text), "%s", text);
    };
    satori::ForwardCard card{};
    satori::ForwardError error{};

    // Headlines the way WeChat words them.
    Fill(0, "u1", "Alice", "hi");
    Check(satori::ForwardBuild(entries, 1, "", false, 1790000000, &card, &error), "build one line");
    Eq(card.title, "Alice的聊天记录", "one speaker");
    free(card.record);
    Fill(1, "u2", "Bob", "yo");
    Check(satori::ForwardBuild(entries, 2, "", false, 1790000000, &card, &error), "build two lines");
    Eq(card.title, "Alice与Bob的聊天记录", "two speakers");
    free(card.record);
    Fill(2, "u1", "Alice", "again");
    Check(satori::ForwardBuild(entries, 3, "", false, 1790000000, &card, &error), "build three lines, two speakers");
    Eq(card.title, "Alice与Bob的聊天记录", "a speaker counts once");
    free(card.record);
    Check(satori::ForwardBuild(entries, 2, "", true, 1790000000, &card, &error), "build for a group");
    Eq(card.title, "群聊的聊天记录", "a group gets the group headline");
    free(card.record);
    Fill(2, "u3", "Carol", "hey");
    Check(satori::ForwardBuild(entries, 3, "", false, 1790000000, &card, &error), "build three speakers");
    Eq(card.title, "群聊的聊天记录", "three speakers are a group chat");
    free(card.record);
    Check(satori::ForwardBuild(entries, 3, "日报", true, 1790000000, &card, &error), "build with a title");
    Eq(card.title, "日报", "the caller's title wins");
    free(card.record);

    // Preview: four lines, one line each, long text cut.
    Fill(0, "u1", "Alice", "first\nsecond line");
    Fill(1, "u2", "Bob", "b");
    Fill(2, "u1", "Alice", "c");
    Fill(3, "u2", "Bob", "d");
    Fill(4, "u1", "Alice", "e");
    Check(satori::ForwardBuild(entries, 5, "", false, 1790000000, &card, &error), "build five lines");
    Eq(card.desc, "Alice: first second line\nBob: b\nAlice: c\nBob: d", "four preview lines, breaks folded");
    free(card.record);
    char long_text[300] = {};
    for (int i = 0; i < 60; ++i) strcat(long_text, "字");
    Fill(0, "u1", "Alice", long_text);
    Check(satori::ForwardBuild(entries, 1, "", false, 1790000000, &card, &error), "build a long line");
    Check(strstr(card.desc, "\xE2\x80\xA6") != nullptr && strlen(card.desc) < 180, "a long line is cut in the preview");
    free(card.record);

    // The record reads back as what was written, through the CDATA of the appmsg around it.
    Fill(0, "u1", "Alice <A&B>", "if a < b && c > d\nthen \"quote\" ]]> and <![CDATA[ x");
    snprintf(entries[0].author_avatar, sizeof(entries[0].author_avatar), "https://wx.example/a?x=1&y=2");
    entries[0].created_s = 1790686908;
    entries[0].svr_id = 6937305540124271325LL;
    Fill(1, "u2", "Bob", "plain");
    Check(satori::ForwardBuild(entries, 2, "", false, 1790690000, &card, &error), "build the round-trip card");
    if (card.record) {
        Check(strstr(card.record, "]]>") == nullptr, "the record never contains a CDATA terminator");
        Check(!strncmp(card.record, "<recordinfo>", 12), "it is a <recordinfo>");
        // Wrap it the way the sender does, then read it back the way a receiver does.
        const size_t size = strlen(card.record) + 128;
        char *appmsg = static_cast<char *>(malloc(size));
        snprintf(appmsg, size, "<msg><appmsg><type>19</type><recorditem><![CDATA[%s]]></recorditem></appmsg></msg>",
                 card.record);
        char *inner = static_cast<char *>(malloc(size));
        Check(satori::XmlGetText(satori::XmlDocument(appmsg), "msg/appmsg/recorditem", inner, size),
              "the recorditem is found");
        Check(!strcmp(inner, card.record), "the CDATA gives the record back unchanged");
        satori::XmlSlice list;
        Check(satori::XmlPath(satori::XmlDocument(inner), "recordinfo/datalist", &list), "a datalist");
        satori::XmlSlice item, attrs;
        char text[512];
        Check(satori::XmlChildAt(list, "dataitem", 0, &item, &attrs), "first item");
        Check(satori::XmlAttribute(attrs, "datatype", text, sizeof(text)) && !strcmp(text, "1"), "a text item");
        Check(satori::XmlAttribute(attrs, "dataid", text, sizeof(text)) && strlen(text) == 32, "with a 32-digit id");
        Check(satori::XmlGetText(item, "datadesc", text, sizeof(text)), "its text");
        Eq(text, "if a < b && c > d\nthen \"quote\" ]]> and <![CDATA[ x", "text with markup and line breaks survives");
        Check(satori::XmlGetText(item, "sourcename", text, sizeof(text)), "its author");
        Eq(text, "Alice <A&B>", "author name survives");
        Check(satori::XmlGetText(item, "sourceheadurl", text, sizeof(text)), "its avatar");
        Eq(text, "https://wx.example/a?x=1&y=2", "avatar URL survives");
        Check(satori::XmlGetText(item, "srcMsgCreateTime", text, sizeof(text)), "its time");
        Eq(text, "1790686908", "an embedded message keeps its own time");
        Check(satori::XmlGetText(item, "fromnewmsgid", text, sizeof(text)), "its server id");
        Eq(text, "6937305540124271325", "server id survives");
        Check(satori::XmlGetText(item, "sourcetime", text, sizeof(text)) && strlen(text) == 19 && text[4] == '-' &&
                  text[10] == ' ',
              "a readable time");
        Check(satori::XmlChildAt(list, "dataitem", 1, &item, &attrs), "second item");
        Check(satori::XmlGetText(item, "srcMsgCreateTime", text, sizeof(text)), "its time");
        Eq(text, "1790690000", "a written line is stamped with the send time");
        Check(!satori::XmlGetText(item, "sourceheadurl", text, sizeof(text)), "no avatar, no element");
        Check(!satori::XmlGetText(item, "fromnewmsgid", text, sizeof(text)), "no server id, no element");
        Check(satori::XmlPath(satori::XmlDocument(inner), "recordinfo/title", &item) &&
                  satori::XmlGetText(satori::XmlDocument(inner), "recordinfo/desc", text, sizeof(text)),
              "title and desc");
        free(inner);
        free(appmsg);
        free(card.record);
    }

    // The record is capped.
    for (int i = 0; i < 100; ++i) {
        char body[4001];
        memset(body, 'x', 4000);
        body[4000] = 0;
        Fill(i, "u1", "Alice", body);
    }
    Check(!satori::ForwardBuild(entries, 100, "", false, 1790000000, &card, &error) &&
              !strcmp(error.code, "forward_too_large"),
          "a record over 256 KiB is refused");

    // The reply echoes the container.
    Fill(0, "u1", "Alice", "a < b");
    snprintf(entries[0].author_avatar, sizeof(entries[0].author_avatar), "https://e.com/a");
    Fill(1, "", "", "anon");
    char *content = satori::ForwardContent(entries, 2, "T\"1");
    Check(content != nullptr, "content");
    if (content) {
        Eq(content,
           "<message forward title=\"T&quot;1\"><message><author id=\"u1\" name=\"Alice\" avatar=\"https://e.com/a\"/>a &lt; b</message>"
           "<message><author name=\"微信用户\"/>anon</message></message>",
           "the echoed container");
        free(content);
    }
    free(entries);

    if (failures) {
        fprintf(stderr, "forward tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("forward tests: PASS\n");
    return 0;
}
