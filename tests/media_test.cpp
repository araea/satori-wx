// Known-answer tests for the hashes and the media link signer, plus the XML scanner.
#include "media.h"
#include "xml_lite.h"
#include "textbuf.h"
#include <stdio.h>
#include <string.h>

namespace {
int failures = 0;
void Check(bool ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
void HexOf(const unsigned char *bytes, size_t n, char *out) {
    for (size_t i = 0; i < n; ++i) snprintf(out + i * 2, 3, "%02x", bytes[i]);
}
bool Text(const char *xml, const char *path, const char *expected) {
    char out[256];
    return satori::XmlGetText(satori::XmlDocument(xml), path, out, sizeof(out)) && !strcmp(out, expected);
}
} // namespace

int main() {
    char md5[33];
    satori::Md5Hex("", 0, md5);
    Check(!strcmp(md5, "d41d8cd98f00b204e9800998ecf8427e"), "md5 empty");
    satori::Md5Hex("abc", 3, md5);
    Check(!strcmp(md5, "900150983cd24fb0d6963f7d28e17f72"), "md5 abc");
    // The voice-file directory is md5 of this name, checked against a real device listing.
    satori::Md5Hex("amr_44083309252650814af57ce101", strlen("amr_44083309252650814af57ce101"), md5);
    Check(!strcmp(md5, "3096e4d3a2d70cfc2e7c7db23570271a"), "md5 of a real voice name");
    char big[1000];
    memset(big, 'a', sizeof(big));
    satori::Md5Hex(big, sizeof(big), md5);  // crosses several 64-byte blocks
    Check(!strcmp(md5, "cabe45dcc9ae5b66ba86600cca6b8ba8"), "md5 long input");

    unsigned char digest[32];
    char hex[65];
    satori::Sha256("abc", 3, digest); HexOf(digest, 32, hex);
    Check(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256 abc");
    satori::Sha256("", 0, digest); HexOf(digest, 32, hex);
    Check(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "sha256 empty");
    // RFC 4231 test cases 1 and 2.
    unsigned char key1[20];
    memset(key1, 0x0b, sizeof(key1));
    satori::HmacSha256(key1, 20, "Hi There", 8, digest); HexOf(digest, 32, hex);
    Check(!strcmp(hex, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"), "hmac rfc4231 #1");
    satori::HmacSha256("Jefe", 4, "what do ya want for nothing?", 28, digest); HexOf(digest, 32, hex);
    Check(!strcmp(hex, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "hmac rfc4231 #2");
    unsigned char key131[131];
    memset(key131, 0xaa, sizeof(key131));  // a key longer than the block is hashed first
    satori::HmacSha256(key131, 131, "Test Using Larger Than Block-Size Key - Hash Key First", 54, digest); HexOf(digest, 32, hex);
    Check(!strcmp(hex, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"), "hmac rfc4231 #6");

    // Link signing: bound to login, kind and id; nothing verifies before a secret exists.
    Check(!satori::MediaVerify("u", "image", "1", "0000000000000000"), "no secret, no link verifies");
    satori::MediaSetSecret("token-one-aaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    char sig[satori::kMediaSigSize];
    satori::MediaSign("wxid_a", "image", "3720", sig);
    Check(strlen(sig) == 16, "signature is 16 hex digits");
    Check(satori::MediaVerify("wxid_a", "image", "3720", sig), "own signature verifies");
    Check(!satori::MediaVerify("wxid_b", "image", "3720", sig), "different login");
    Check(!satori::MediaVerify("wxid_a", "video", "3720", sig), "different kind");
    Check(!satori::MediaVerify("wxid_a", "image", "3721", sig), "different id");
    Check(!satori::MediaVerify("wxid_a", "image", "3720", "0000000000000000"), "forged signature");
    Check(!satori::MediaVerify("wxid_a", "image", "3720", ""), "empty signature");
    Check(!satori::MediaVerify("wxid_a", "image", "3720", "abc"), "short signature");
    char link[256];
    Check(satori::MediaLink("wxid_a", "image", "3720", link, sizeof(link)), "link fits");
    char expected[300];
    snprintf(expected, sizeof(expected), "internal:wechat/wxid_a/_msg/image/3720/%s", sig);
    Check(!strcmp(link, expected), "link layout");
    Check(!satori::MediaLink("wxid_a", "image", "3720", link, 20), "link refuses a small buffer");
    satori::MediaSetSecret("token-two-bbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    Check(!satori::MediaVerify("wxid_a", "image", "3720", sig), "rotating the token revokes old links");

    // ---- XML scanner ------------------------------------------------------------------------
    const char *quote =
        "<?xml version=\"1.0\"?>\n<msg>\n\t<appmsg appid=\"\" sdkver=\"0\">\n\t\t<title>回复 &amp; 内容</title>\n\t\t<type>57</type>\n"
        "\t\t<refermsg>\n\t\t\t<type>1</type>\n\t\t\t<svrid>123456789</svrid>\n\t\t\t<title>不该被选中</title>\n"
        "\t\t\t<displayname>甲</displayname>\n\t\t\t<content>原文\n第二行</content>\n\t\t</refermsg>\n\t</appmsg>\n</msg>";
    Check(Text(quote, "msg/appmsg/title", "回复 & 内容"), "direct child title, entity decoded");
    Check(Text(quote, "msg/appmsg/type", "57"), "appmsg type is the first <type>, not the nested one");
    Check(Text(quote, "msg/appmsg/refermsg/type", "1"), "nested type");
    Check(Text(quote, "msg/appmsg/refermsg/svrid", "123456789"), "svrid");
    Check(Text(quote, "msg/appmsg/refermsg/content", "原文\n第二行"), "multi-line content");
    Check(!Text(quote, "msg/appmsg/refermsg/nothing", ""), "missing element");
    Check(!Text(quote, "appmsg/title", ""), "path must start at the root element");
    char attr[64];
    Check(satori::XmlGetAttribute(satori::XmlDocument(quote), "msg/appmsg", "sdkver", attr, sizeof(attr)) && !strcmp(attr, "0"), "attribute");
    Check(satori::XmlGetAttribute(satori::XmlDocument(quote), "msg/appmsg", "appid", attr, sizeof(attr)) && !*attr, "empty attribute");
    Check(!satori::XmlGetAttribute(satori::XmlDocument(quote), "msg/appmsg", "nope", attr, sizeof(attr)), "missing attribute");

    Check(Text("<msg><a><![CDATA[x < y && <b>]]></a></msg>", "msg/a", "x < y && <b>"), "CDATA kept verbatim");
    Check(Text("<msg><a>  <![CDATA[两段]]>和<![CDATA[三段]]> </a></msg>", "msg/a", "两段和三段"), "CDATA runs concatenate");
    Check(Text("<msg><a>&#x4f60;&#22909;&lt;&#38;</a></msg>", "msg/a", "你好<&"), "numeric character references");
    Check(Text("<msg><!-- <a>no</a> --><a>yes</a></msg>", "msg/a", "yes"), "comment skipped");
    Check(Text("<msg><b><a>inner</a></b><a>outer</a></msg>", "msg/a", "outer"), "nested same-name is not a direct child");
    Check(Text("<msg><a/><b>x</b></msg>", "msg/a", ""), "self-closing element is empty");
    Check(Text("<msg><a>unterminated</msg>", "msg/a", "") == false || true, "malformed input must not crash");
    Check(satori::XmlGetAttribute(satori::XmlDocument("<msg><img aeskey='k>1' cdnthumburl=\"http://x/y?a=1&amp;b=2\" w=5 /></msg>"), "msg/img", "cdnthumburl", attr, sizeof(attr)) && !strcmp(attr, "http://x/y?a=1&b=2"), "quoted attribute containing >, entity decoded");
    Check(satori::XmlGetAttribute(satori::XmlDocument("<msg><img aeskey='k>1' w=5 /></msg>"), "msg/img", "w", attr, sizeof(attr)) && !strcmp(attr, "5"), "unquoted attribute after a quoted '>'");
    // Truncation never splits a character.
    char small[5];
    satori::XmlGetText(satori::XmlDocument("<m><a>你好世界</a></m>"), "m/a", small, sizeof(small));
    Check(!strcmp(small, "你"), "truncation on a UTF-8 boundary");
    // A group-chat sysmsg like the ones WeChat writes for a join.
    const char *sys = "<sysmsg type=\"sysmsgtemplate\"><sysmsgtemplate><content_template type=\"tmpl_type_profile\">"
                      "<template><![CDATA[\"$username$\"邀请你加入了群聊]]></template><link_list><link name=\"username\" type=\"link_profile\">"
                      "<memberlist><member><username><![CDATA[wxid_x]]></username><nickname><![CDATA[甲]]></nickname></member></memberlist>"
                      "</link></link_list></content_template></sysmsgtemplate></sysmsg>";
    Check(satori::XmlGetAttribute(satori::XmlDocument(sys), "sysmsg", "type", attr, sizeof(attr)) && !strcmp(attr, "sysmsgtemplate"), "sysmsg type");
    Check(Text(sys, "sysmsg/sysmsgtemplate/content_template/link_list/link/memberlist/member/username", "wxid_x"), "deep path");

    satori::TextBuf buffer;
    buffer.Append("a<");
    buffer.Text("<&>\"");
    buffer.Attr("<&>\"");
    Check(buffer.size > 0 && !strcmp(buffer.data, "a<&lt;&amp;&gt;\"&lt;&amp;&gt;&quot;"), "TextBuf escapes");
    char *taken = buffer.Take();
    Check(taken && !buffer.data && buffer.size == 0, "Take resets the buffer");
    free(taken);
    satori::TextBuf empty;
    char *nothing = empty.Take();
    Check(nothing && !*nothing, "Take of an untouched buffer is an empty string");
    free(nothing);

    if (failures) { fprintf(stderr, "%d media/xml test(s) failed\n", failures); return 1; }
    printf("media tests: PASS\n");
    return 0;
}
