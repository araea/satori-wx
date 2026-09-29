// Host-side tests for flattening a Satori `content` string into the plain text WeChat
// can actually send. No device, no Java, no cJSON objects are involved.
#include "protocol.h"
#include <stdio.h>
#include <string.h>

namespace {
int failures = 0;

void Check(bool ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

// Runs PlainText into a roomy buffer and compares the result.
void Eq(const char *content, const char *expected, const char *what) {
    char out[512];
    satori::PlainText(content, out, sizeof(out));
    if (strcmp(out, expected)) {
        fprintf(stderr, "FAIL: %s\n  in:  %s\n  got: %s\n  want: %s\n", what, content, out, expected);
        ++failures;
    }
}

// Collects ImageSources and compares the joined list against a '|'-separated expectation.
void Img(const char *content, const char *expected, const char *what) {
    char sources[4][satori::kImageSrcMax];
    const size_t count = satori::ImageSources(content, sources, 4);
    char joined[4 * satori::kImageSrcMax + 4] = {};
    for (size_t i = 0; i < count; ++i) {
        if (i) strcat(joined, "|");
        strncat(joined, sources[i], sizeof(joined) - strlen(joined) - 1);
    }
    if (strcmp(joined, expected)) {
        fprintf(stderr, "FAIL: %s\n  in:  %s\n  got: %s\n  want: %s\n", what, content, joined, expected);
        ++failures;
    }
}
}

int main() {
    // Escaped text is the payload; entities come back as the characters they stand for.
    Eq("a &lt; b &amp; c &gt; d &quot;e&quot;", "a < b & c > d \"e\"", "entities unescape");
    Eq("caf\u00e9 \u4f60\u597d \U0001f600", "caf\u00e9 \u4f60\u597d \U0001f600", "utf-8 passes through");

    // Elements a text-only adapter cannot carry are dropped, not printed.
    Eq("<quote id=\"123\"/><at id=\"456\"/>你好", "你好", "quote and at drop");
    Eq("<at type=\"all\"/>全体注意", "全体注意", "at all drops");
    Eq("<img src=\"https://x/y.png\"/>看图", "看图", "image drops");
    Eq("<emoji id=\"14\"/>", "", "element-only content is empty");
    Eq("<message><author id=\"1\" name=\"A\"/>正文</message>", "正文", "forward keeps inner text");
    Eq("<message forward=\"true\" id=\"9\"/>", "", "forward reference drops");

    // <br/> is the one tag that survives as text.
    Eq("第一行<br/>第二行", "第一行\n第二行", "br becomes newline");
    Eq("a<br>b", "a\nb", "br without slash");
    Eq("</message>收起", "收起", "closing tag drops");

    // A '>' inside a quoted attribute must not end the tag early.
    Eq("<img src=\"a>b\"/>hi", "hi", "quoted angle bracket");
    Eq("<quote id=\"7'/>&lt;\"/>x", "x", "quoted apostrophe");

    // An unterminated tag is not guesswork; drop the unfinished tail.
    Eq("正文<quote id=\"1", "正文", "unterminated tag");

    // Bounded output: never overruns, always NUL-terminated.
    char small[4];
    const size_t n = satori::PlainText("abcdef", small, sizeof(small));
    Check(n == 3 && !strcmp(small, "abc"), "output is bounded and terminated");
    char one[1] = {'x'};
    Check(satori::PlainText("abc", one, sizeof(one)) == 0 && one[0] == 0, "capacity 1 yields empty");

    // Image sources are collected so message.create can tell "this is only a picture" from
    // "this is empty"; only <img> counts, and an attribute's '>' must not end the tag early.
    Img("", "", "no images");
    Img("纯文本", "", "text only");
    Img("<img src=\"internal:wechat/u/_tmp/a.png\"/>", "internal:wechat/u/_tmp/a.png", "single image");
    Img("<at id=\"1\"/><img src=\"a\"/><img src='b'/>看<img src=\"c\">",
        "a|b|c", "several images, mixed quotes");
    Img("<img src=\"x>y\"/>", "x>y", "quoted angle bracket");
    Img("<img width=\"1\" src=\"z\"/>", "z", "src after another attribute");
    Img("<img src=\"\"/>", "", "empty src drops");
    Img("<img src=\"a\"", "", "unterminated tag drops");
    Img("<image src=\"a\"/>", "", "only the img element counts");
    {
        char sources[4][satori::kImageSrcMax];
        Check(satori::ImageSources("<img src=\"a\"/><img src=\"b\"/>", sources, 1) == 1 &&
              !strcmp(sources[0], "a"), "max bounds the result");
    }

    // ---- OutgoingText: mentions and links ---------------------------------------------------------------
    {
        char out[512];
        satori::OutgoingMention mentions[4];
        size_t count = 0;
        satori::OutgoingText("<at id=\"wxid_a\" name=\"甲\"/> 你好 <at id=\"wxid_b\" name=\"乙&amp;丙\"/>收到吗", out, sizeof(out), mentions, 4, &count, nullptr, nullptr);
        Check(!strcmp(out, "@甲\xE2\x80\x85 你好 @乙&丙\xE2\x80\x85收到吗"), "mentions become @name + U+2005");
        Check(count == 2 && !strcmp(mentions[0].id, "wxid_a") && !strcmp(mentions[1].id, "wxid_b") && !strcmp(mentions[1].name, "乙&丙"), "mention list, entity-decoded, in order");
        // No name: the namer supplies one, and without a namer the id stands in.
        struct Namer { static bool Name(void *, const char *id, char *name, size_t capacity) { snprintf(name, capacity, "昵称-%s", id); return true; } };
        satori::OutgoingText("<at id=\"wxid_c\"/>hi", out, sizeof(out), mentions, 4, &count, Namer::Name, nullptr);
        Check(!strcmp(out, "@昵称-wxid_c\xE2\x80\x85hi") && count == 1, "a namer resolves a missing name");
        satori::OutgoingText("<at id=\"wxid_c\"/>hi", out, sizeof(out), mentions, 4, &count, nullptr, nullptr);
        Check(!strcmp(out, "@wxid_c\xE2\x80\x85hi"), "without a namer the id is shown");
        satori::OutgoingText("<at type=\"all\"/>开会", out, sizeof(out), mentions, 4, &count, nullptr, nullptr);
        Check(!strcmp(out, "@所有人\xE2\x80\x85开会") && count == 1 && !strcmp(mentions[0].id, "notify@all"), "@all");
        satori::OutgoingText("<at type=\"here\"/>x<at/>y", out, sizeof(out), mentions, 4, &count, nullptr, nullptr);
        Check(!strcmp(out, "xy") && count == 0, "audiences WeChat lacks, and an <at> without an id, are dropped");
        satori::OutgoingText("<at id=\"1\" name=\"a\"/><at id=\"2\" name=\"b\"/><at id=\"3\" name=\"c\"/>", out, sizeof(out), mentions, 2, &count, nullptr, nullptr);
        Check(count == 2, "the mention list is bounded");
        // Links keep their target.
        satori::OutgoingText("看<a href=\"https://x.y/z?a=1&amp;b=2\">这里</a>吧", out, sizeof(out), nullptr, 0, nullptr, nullptr, nullptr);
        Check(!strcmp(out, "看这里 (https://x.y/z?a=1&b=2)吧"), "a link keeps its target after the words");
        satori::OutgoingText("<a href=\"https://x.y\">https://x.y</a>", out, sizeof(out), nullptr, 0, nullptr, nullptr, nullptr);
        Check(!strcmp(out, "https://x.y"), "a link whose text is the target is not repeated");
        satori::OutgoingText("<a>没有目标</a>", out, sizeof(out), nullptr, 0, nullptr, nullptr, nullptr);
        Check(!strcmp(out, "没有目标"), "an anchor without href is just text");
        satori::OutgoingText("<at id=\"wxid_a\" name=\"甲\"/>x", out, sizeof(out), nullptr, 0, nullptr, nullptr, nullptr);
        Check(!strcmp(out, "x"), "PlainText still drops mentions");
    }

    // ---- ImageSpans / TagAttribute / Base64Decode -----------------------------------------------------------
    {
        const char *content = "看<img src=\"a&amp;b\"/>中<IMG width=\"1\" src='c'>末<image src=\"no\"/>";
        satori::ImageSpan spans[4];
        const size_t count = satori::ImageSpans(content, spans, 4);
        Check(count == 2, "two <img> elements (not <image>)");
        if (count == 2) {
            Check(!strncmp(content + spans[0].begin, "<img src=\"a&amp;b\"/>", spans[0].end - spans[0].begin), "first span covers the whole tag");
            Check(!strncmp(content + spans[1].begin, "<IMG width=\"1\" src='c'>", spans[1].end - spans[1].begin), "second span, upper case and mixed quotes");
            char src[64];
            Check(satori::TagAttribute(content, spans[0], "src", src, sizeof(src)) && !strcmp(src, "a&b"), "src is entity-decoded");
            Check(satori::TagAttribute(content, spans[1], "src", src, sizeof(src)) && !strcmp(src, "c"), "src after another attribute");
            Check(!satori::TagAttribute(content, spans[1], "alt", src, sizeof(src)), "missing attribute");
        }
        Check(satori::ImageSpans("<img src=\"a\"", spans, 4) == 0, "an unterminated tag is not a span");
        Check(satori::ImageSpans("<img/><img/><img/>", spans, 2) == 2, "spans are bounded");

        unsigned char out[64];
        long n = satori::Base64Decode("aGVsbG8gd29ybGQ=", 16, out, sizeof(out));
        Check(n == 11 && !memcmp(out, "hello world", 11), "base64");
        n = satori::Base64Decode("aGVs\nbG8", 8, out, sizeof(out));
        Check(n == 5 && !memcmp(out, "hello", 5), "whitespace ignored, padding optional");
        n = satori::Base64Decode("-_-_", 4, out, sizeof(out));
        Check(n == 3 && out[0] == 0xFB, "url-safe alphabet");
        Check(satori::Base64Decode("a$b=", 4, out, sizeof(out)) == -1, "a character outside the alphabet");
        Check(satori::Base64Decode("aGVsbG8gd29ybGQ=", 16, out, 4) == -1, "output capacity is enforced");
        Check(satori::Base64Decode("", 0, out, sizeof(out)) == 0, "empty input");
    }

    // ---- MediaSpans / FirstTag ----------------------------------------------------------------------------
    {
        const char *content = "<quote id=\"1\"/>看<img src=\"a\"/>听<AUDIO src=\"b\"></audio>播<video src=\"c\" poster=\"d\"/>存<file title=\"x>y\" src=\"e\"/>末<image src=\"no\"/><filename/>";
        satori::MediaSpan spans[8];
        const size_t count = satori::MediaSpans(content, spans, 8);
        Check(count == 4, "four media elements: img, audio, video, file (not quote, image or filename)");
        if (count == 4) {
            Check(spans[0].kind == 'i' && spans[1].kind == 'a' && spans[2].kind == 'v' && spans[3].kind == 'f', "kinds, in order");
            const satori::ImageSpan file{spans[3].begin, spans[3].end};
            char title[32], src[32];
            Check(satori::TagAttribute(content, file, "title", title, sizeof(title)) && !strcmp(title, "x>y"), "a '>' inside a quoted attribute stays inside the tag");
            Check(satori::TagAttribute(content, file, "src", src, sizeof(src)) && !strcmp(src, "e"), "attributes after it are still found");
        }
        Check(satori::MediaSpans("<audio src=\"a\"></audio>", spans, 8) == 1, "a closing tag is not a second element");
        Check(satori::MediaSpans("<video/><video/><video/>", spans, 2) == 2, "media spans are bounded");
        satori::ImageSpan quote;
        Check(satori::FirstTag(content, "quote", &quote) && quote.begin == 0, "the quote element is found");
        char id[16];
        Check(satori::TagAttribute(content, quote, "id", id, sizeof(id)) && !strcmp(id, "1"), "and its id");
        Check(!satori::FirstTag("<quotes id=\"1\"/></quote>", "quote", &quote), "a longer name or a closing tag is not a quote");
        Check(satori::FirstTag("字<QUOTE id='9'>", "quote", &quote) && quote.begin == strlen("字"), "case-insensitive, after other text");
    }

    if (failures) { fprintf(stderr, "%d content test(s) failed\n", failures); return 1; }
    printf("content tests: PASS\n");
    return 0;
}
