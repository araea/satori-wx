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

    if (failures) { fprintf(stderr, "%d content test(s) failed\n", failures); return 1; }
    printf("content tests: PASS\n");
    return 0;
}
