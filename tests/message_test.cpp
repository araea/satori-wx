// Host tests for the row -> Satori content decoder. Every sample is shaped like a row observed
// on a real WeChat 8.0.78 database (headers, CDATA, the odd type numbers), with the personal
// content replaced.
#include "media.h"
#include "wx_message.h"
#include <stdio.h>
#include <string.h>

namespace {
using satori::Decoded;
using satori::MessageRow;
using satori::MsgKind;
int failures = 0;
void Check(bool ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
bool Eq(const char *actual, const char *expected, const char *what) {
    const bool ok = actual && !strcmp(actual, expected);
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n  got:      %s\n  expected: %s\n", what, actual ? actual : "(null)", expected);
        ++failures;
    }
    return ok;
}
const char *kSelf = "wxid_self";

MessageRow Row(long long id, int type, const char *talker, const char *content, int is_send = 0) {
    MessageRow row;
    row.id = id;
    row.type = type;
    row.talker = talker;
    row.content = content;
    row.is_send = is_send;
    return row;
}
bool Contains(const char *text, const char *part) { return text && strstr(text, part); }
} // namespace

int main() {
    satori::MediaSetSecret("token-for-the-message-tests-aaaaaaaaaaaa");
    Decoded d;

    // ---- text -------------------------------------------------------------------------------
    satori::DecodeMessage(Row(1, 1, "wxid_friend", "a<b>&c \"q\""), kSelf, &d);
    Check(d.deliver && d.kind == MsgKind::Text, "private text delivers");
    Eq(d.content, "a&lt;b&gt;&amp;c \"q\"", "private text is escaped, not the header-split");
    Check(!*d.sender, "private text has no header sender");
    // A colon at the start of a private message is content, never a header.
    satori::DecodeMessage(Row(2, 1, "wxid_friend", "note: hello\nworld"), kSelf, &d);
    Eq(d.content, "note: hello\nworld", "private colon kept");

    satori::DecodeMessage(Row(3, 1, "123@chatroom", "wxid_a:\n你好"), kSelf, &d);
    Eq(d.sender, "wxid_a", "group header sender");
    Eq(d.content, "你好", "group header stripped");
    // My own group message has no header.
    satori::DecodeMessage(Row(4, 1, "123@chatroom", "note: hi", 1), kSelf, &d);
    Eq(d.content, "note: hi", "own group message untouched");

    // ---- mentions ---------------------------------------------------------------------------
    MessageRow at = Row(5, 1, "123@chatroom", "wxid_a:\n@乙\xE2\x80\x85 看下这个 @丙\xE2\x80\x85才对");
    at.msg_source = "<msgsource>\n\t<atuserlist><![CDATA[wxid_b,wxid_c]]></atuserlist>\n\t<pua>1</pua>\n</msgsource>";
    satori::DecodeMessage(at, kSelf, &d);
    Eq(d.content, "<at id=\"wxid_b\" name=\"乙\"/> 看下这个 <at id=\"wxid_c\" name=\"丙\"/> 才对",
       "two mentions become <at>");
    MessageRow all = Row(6, 1, "123@chatroom", "wxid_a:\n@所有人\xE2\x80\x85开会");
    all.msg_source = "<msgsource><atuserlist>notify@all</atuserlist></msgsource>";
    satori::DecodeMessage(all, kSelf, &d);
    Eq(d.content, "<at type=\"all\"/> 开会", "@all");
    // Names are not compared: a group alias in the text still maps by order.
    MessageRow alias = Row(7, 1, "123@chatroom", "wxid_a:\n@群里的叫法\xE2\x80\x85 hi");
    alias.msg_source = "<msgsource><atuserlist><![CDATA[wxid_z]]></atuserlist></msgsource>";
    satori::DecodeMessage(alias, kSelf, &d);
    Eq(d.content, "<at id=\"wxid_z\" name=\"群里的叫法\"/> hi", "alias keeps the visible name");
    // More ids than visible mentions: none may be lost.
    MessageRow extra = Row(8, 1, "123@chatroom", "wxid_a:\n@甲\xE2\x80\x85 只剩一个");
    extra.msg_source = "<msgsource><atuserlist><![CDATA[wxid_1,wxid_2]]></atuserlist></msgsource>";
    satori::DecodeMessage(extra, kSelf, &d);
    Eq(d.content, "<at id=\"wxid_2\"/> <at id=\"wxid_1\" name=\"甲\"/> 只剩一个",
       "leftover mention ids are kept in front");
    // A typed "@" without the picker's separator is just text.
    MessageRow typed = Row(9, 1, "123@chatroom", "wxid_a:\n@乙 hi");
    typed.msg_source = "<msgsource><atuserlist><![CDATA[wxid_b]]></atuserlist></msgsource>";
    satori::DecodeMessage(typed, kSelf, &d);
    Eq(d.content, "<at id=\"wxid_b\"/> @乙 hi", "no separator, no in-place mention");
    // Mentions in a private chat do not exist, whatever the source says.
    MessageRow direct = Row(10, 1, "wxid_friend", "@乙\xE2\x80\x85 hi");
    direct.msg_source = "<msgsource><atuserlist>wxid_b</atuserlist></msgsource>";
    satori::DecodeMessage(direct, kSelf, &d);
    Check(!Contains(d.content, "<at"), "no mentions in a private chat");
    // An attribute-hostile name is escaped.
    MessageRow hostile = Row(11, 1, "123@chatroom", "wxid_a:\n@\"><b>\xE2\x80\x85 x");
    hostile.msg_source = "<msgsource><atuserlist>wxid_b</atuserlist></msgsource>";
    satori::DecodeMessage(hostile, kSelf, &d);
    Eq(d.content, "<at id=\"wxid_b\" name=\"&quot;&gt;&lt;b&gt;\"/> x", "mention name escaped");

    // ---- media ------------------------------------------------------------------------------
    char link[256], expected[512];
    satori::MediaLink(kSelf, "image", "3720", link, sizeof(link));
    snprintf(expected, sizeof(expected), "<img src=\"%s\"/>", link);
    satori::DecodeMessage(
        Row(3720, 3, "123@chatroom",
            "wxid_a:\n<?xml version=\"1.0\"?>\n<msg>\n\t<img aeskey=\"k\" cdnthumburl=\"u\" />\n</msg>"),
        kSelf, &d);
    Check(d.kind == MsgKind::Image, "image kind");
    Eq(d.content, expected, "image element with a signed link");
    Eq(d.sender, "wxid_a", "image sender");
    satori::DecodeMessage(Row(3720, 3, "wxid_f", "<msg><img /></msg>"), nullptr, &d);
    Eq(d.content, "[图片]", "no login: placeholder, not an unverifiable link");

    satori::MediaLink(kSelf, "voice", "4015", link, sizeof(link));
    snprintf(expected, sizeof(expected), "<audio src=\"%s\" duration=\"4.746\"/>", link);
    satori::DecodeMessage(
        Row(4015, 34, "123@chatroom",
            "wxid_r: <msg><voicemsg endflag=\"1\" voiceformat=\"4\" voicelength=\"4746\" length=\"7585\" /></msg>"),
        kSelf, &d);
    Eq(d.content, expected, "voice with the ': ' header and duration in seconds");
    satori::DecodeMessage(Row(4016, 34, "wxid_f", "<msg><voicemsg voicelength=\"5000\" /></msg>"), kSelf, &d);
    Check(Contains(d.content, "duration=\"5\""), "whole seconds have no decimals");
    // A voice we sent ourselves is stored as "<wxid>:<milliseconds>:<flag>" until WeChat rewrites it.
    MessageRow own = Row(4468, 34, "filehelper", "wxid_me:4746:0\n");
    own.is_send = true;
    satori::DecodeMessage(own, kSelf, &d);
    satori::MediaLink(kSelf, "voice", "4468", link, sizeof(link));
    snprintf(expected, sizeof(expected), "<audio src=\"%s\" duration=\"4.746\"/>", link);
    Eq(d.content, expected, "our own voice row carries its length too");

    // Video: "sender:<seconds>:<flag>" and no body at all.
    char poster[256];
    satori::MediaLink(kSelf, "video", "3390", link, sizeof(link));
    satori::MediaLink(kSelf, "videothumb", "3390", poster, sizeof(poster));
    snprintf(expected, sizeof(expected), "<video src=\"%s\" poster=\"%s\" duration=\"7\"/>", link, poster);
    satori::DecodeMessage(Row(3390, 43, "123@chatroom", "wxid_a:7:0\n"), kSelf, &d);
    Check(d.kind == MsgKind::Video, "video kind");
    Eq(d.content, expected, "video from the short header");
    Eq(d.sender, "wxid_a", "video sender");
    satori::DecodeMessage(Row(3752, 43, "sunzhiqin", "sunzhiqin:0:0"), kSelf, &d);
    Check(Contains(d.content, "duration=\"0\""), "private video header too");

    // Stickers: header "sender:0:1:<md5>:sender*#*\n<msg>", the URL is masked with *#*.
    satori::DecodeMessage(
        Row(4099, 47, "123@chatroom",
            "wxid_a:0:1:5c3f:wxid_a*#*\n<msg><emoji fromusername=\"wxid_a\" md5=\"5c3f\" cdnurl=\"http*#*//vweixinf.tc.qq.com/110/x?m=5c3f&amp;hy=SH\" /></msg>"),
        kSelf, &d);
    Check(d.kind == MsgKind::Emoji, "emoji kind");
    Eq(d.content, "<img src=\"http://vweixinf.tc.qq.com/110/x?m=5c3f&amp;hy=SH\"/>",
       "sticker URL unmasked and attribute-escaped");
    Eq(d.sender, "wxid_a", "sticker sender");
    satori::DecodeMessage(Row(4100, 47, "wxid_f", "<msg><emoji md5=\"aa\" /></msg>", 1), kSelf, &d);
    Check(Contains(d.content, "_msg/emoji/4100/"), "no CDN url: falls back to a signed local link");

    satori::DecodeMessage(
        Row(48, 48, "wxid_f",
            "<?xml version=\"1.0\"?>\n<msg><location x=\"30.318007\" y=\"121.246338\" label=\"某路129号\" poiname=\"某某学校\" /></msg>"),
        kSelf, &d);
    Eq(d.content, "[位置] 某某学校（某路129号） 30.318007,121.246338", "location");
    satori::DecodeMessage(Row(42, 42, "wxid_f", "<msg username=\"wxid_card\" nickname=\"名片昵称\" /></msg>"), kSelf,
                          &d);
    Eq(d.content, "[名片] 名片昵称 (wxid_card)", "card");

    // ---- appmsg -----------------------------------------------------------------------------
    satori::DecodeMessage(
        Row(100, 285212721, "gh_service",
            "<msg>\n<appmsg appid=\"\" sdkver=\"0\">\n<title><![CDATA[标题 & 一]]></title>\n<des><![CDATA[摘要]]></des>\n<type>5</type>\n"
            "<url><![CDATA[http://mp.weixin.qq.com/s?a=1&b=2]]></url>\n</appmsg>\n</msg>"),
        kSelf, &d);
    Check(d.kind == MsgKind::Link, "link kind");
    Eq(d.content, "<a href=\"http://mp.weixin.qq.com/s?a=1&amp;b=2\">标题 &amp; 一</a>\n摘要", "link with description");
    satori::DecodeMessage(
        Row(101, 49, "gh_service",
            "<msg><appmsg><title>t</title><des>t</des><type>5</type><url>https://x/y</url></appmsg></msg>"),
        kSelf, &d);
    Eq(d.content, "<a href=\"https://x/y\">t</a>", "same description is not repeated");
    // Not everything with a url is safe to link.
    satori::DecodeMessage(
        Row(102, 49, "wxid_f",
            "<msg><appmsg><title>x</title><type>5</type><url>javascript:alert(1)</url></appmsg></msg>"),
        kSelf, &d);
    Eq(d.content, "[x]", "non-http urls are not linked");
    satori::DecodeMessage(
        Row(103, 436207665, "wxid_f",
            "<msg><appmsg><des><![CDATA[我给你发了一个红包，赶紧去拆!]]></des><url><![CDATA[https://wxapp.tenpay.com/x]]></url><type><![CDATA[2001]]></type><title><![CDATA[微信红包]]></title></appmsg></msg>"),
        kSelf, &d);
    Eq(d.content, "[微信红包] 我给你发了一个红包，赶紧去拆!", "red packet is text, its tenpay link is not exposed");

    satori::MediaLink(kSelf, "file", "104", link, sizeof(link));
    snprintf(expected, sizeof(expected), "<file src=\"%s\" title=\"报告.pdf\"/>", link);
    satori::DecodeMessage(
        Row(104, 1090519089, "wxid_f",
            "<msg><appmsg><title>报告.pdf</title><type>6</type><appattach><fileext>pdf</fileext></appattach></appmsg></msg>"),
        kSelf, &d);
    Check(d.kind == MsgKind::File, "file kind");
    Eq(d.content, expected, "file element");

    // Replies: the title is the new text; <refermsg> says what it answers.
    satori::DecodeMessage(
        Row(200, 822083633, "123@chatroom",
            "wxid_a:\n<?xml version=\"1.0\"?>\n<msg>\n<appmsg>\n<title>回复内容</title>\n<type>57</type>\n<refermsg>\n<type>1</type>\n<svrid>6252387697952569056</svrid>\n"
            "<fromusr>123@chatroom</fromusr>\n<chatusr>wxid_orig</chatusr>\n<displayname>原作者</displayname>\n<content>wxid_orig:\n被引用的话</content>\n</refermsg>\n</appmsg>\n</msg>"),
        kSelf, &d);
    Check(d.kind == MsgKind::Quote && d.deliver, "quote kind");
    Eq(d.content, "回复内容", "reply text");
    Eq(d.refer_svr_id, "6252387697952569056", "refer svrid");
    Eq(d.refer_user, "wxid_orig", "refer author is chatusr");
    Eq(d.refer_name, "原作者", "refer display name");
    Eq(d.refer_text, "被引用的话", "refer text loses its header");
    Eq(d.sender, "wxid_a", "quote sender");
    satori::DecodeMessage(
        Row(201, 822083633, "wxid_f",
            "<msg><appmsg><title>看图</title><type>57</type><refermsg><type>3</type><svrid>77</svrid><fromusr>wxid_f</fromusr><content>&lt;msg&gt;&lt;img/&gt;&lt;/msg&gt;</content></refermsg></appmsg></msg>"),
        kSelf, &d);
    Eq(d.refer_text, "[图片]", "a quoted photo quotes as a label");
    Eq(d.refer_user, "wxid_f", "private reply falls back to fromusr");

    // ---- things that are not messages --------------------------------------------------------
    satori::DecodeMessage(Row(300, 570425393, "123@chatroom",
                              "123@chatroom:\n<sysmsg type=\"sysmsgtemplate\"><sysmsgtemplate/></sysmsg>"),
                          kSelf, &d);
    Check(d.kind == MsgKind::System && !d.deliver, "sysmsg carried in an appmsg-looking type");
    satori::DecodeMessage(Row(301, 10000, "wxid_f", "你已添加了某人，现在可以开始聊天了。"), kSelf, &d);
    Check(d.kind == MsgKind::System && !d.deliver, "10000 is a system tip");
    satori::DecodeMessage(Row(302, 268445456, "123@chatroom", "\"某人\" 撤回了一条消息"), kSelf, &d);
    Check(d.kind == MsgKind::Revoke && !d.deliver, "group revoke marker");
    satori::DecodeMessage(Row(303, 285222674, "wxid_f", "你撤回了一条消息"), kSelf, &d);
    Check(d.kind == MsgKind::Revoke, "own revoke marker");
    satori::DecodeMessage(Row(304, 50, "wxid_f", "<voipmsg type=\"VoIPBubbleMsg\"/>"), kSelf, &d);
    Check(!d.deliver && d.kind == MsgKind::Ignored, "call logs are ignored");
    satori::DecodeMessage(Row(305, 49, "wxid_f", "<msg><appmsg><title>x</title><type>62</type></appmsg></msg>"), kSelf,
                          &d);
    Check(!d.deliver && d.kind == MsgKind::System, "a pat wrapper without records is bookkeeping");
    // ---- pats ("拍一戳") ----------------------------------------------------------------------
    // Shaped like a real type-922746929 row (appmsg 62 wrapper around <patMsg>).
    satori::DecodeMessage(
        Row(310, 922746929, "123@chatroom",
            "<msg><appmsg appid=\"\" sdkver=\"0\"><title>当前版本不支持展示该内容，请升级至最新版本。</title><type>62</type>"
            "<patMsg><chatUser>123@chatroom</chatUser><records><recordNum>1</recordNum><record>"
            "<fromUser>wxid_a</fromUser><pattedUser>wxid_self</pattedUser>"
            "<template><![CDATA[\"${wxid_a}\" 拍了拍 \"${wxid_self}\"]]></template>"
            "<createTime>1790611143000</createTime><readStatus>1</readStatus><svrId>4122221325758051183</svrId>"
            "<showModifyTip>0</showModifyTip><isNewPatMsg>1</isNewPatMsg></record></records></patMsg></appmsg></msg>"),
        kSelf, &d);
    Check(d.deliver && d.kind == MsgKind::Other, "a pat record delivers");
    Eq(d.content, "\"wxid_a\" 拍了拍 \"你\"", "pat template expands, self becomes 你");
    Eq(d.sender, "wxid_a", "pat author is the pat-er");
    // Several records in one aggregated row read as several lines.
    satori::DecodeMessage(Row(311, 922746929, "123@chatroom",
                              "<msg><appmsg><type>62</type><patMsg><records><recordNum>2</recordNum>"
                              "<record><fromUser>wxid_a</fromUser><pattedUser>wxid_b</pattedUser>"
                              "<template><![CDATA[\"${wxid_a}\" 拍了拍 \"${wxid_b}\"]]></template></record>"
                              "<record><fromUser>wxid_c</fromUser><pattedUser>wxid_self</pattedUser>"
                              "<template><![CDATA[\"${wxid_c}\" 拍了拍 \"${wxid_self}\"]]></template></record>"
                              "</records></patMsg></appmsg></msg>"),
                          kSelf, &d);
    Check(d.deliver, "an aggregated pat row delivers");
    Eq(d.content, "\"wxid_a\" 拍了拍 \"wxid_b\"\n\"wxid_c\" 拍了拍 \"你\"",
       "aggregated pat records join with a newline");
    Eq(d.sender, "wxid_a", "aggregated pat keeps the first pat-er as author");
    // Our own pat: the row is ours, the patted one is a plain id.
    satori::DecodeMessage(Row(312, 922746929, "wxid_f",
                              "<msg><appmsg><type>62</type><patMsg><records><record>"
                              "<fromUser>wxid_self</fromUser><pattedUser>wxid_f</pattedUser>"
                              "<template><![CDATA[\"${wxid_self}\" 拍了拍 \"${wxid_f}\"]]></template>"
                              "</record></records></patMsg></appmsg></msg>"),
                          kSelf, &d);
    Check(d.deliver, "our own pat delivers");
    Eq(d.content, "\"你\" 拍了拍 \"wxid_f\"", "own pat names itself 你");
    // A record without a template still reads as who patted whom.
    satori::DecodeMessage(Row(313, 922746929, "123@chatroom",
                              "<msg><appmsg><type>62</type><patMsg><records><record>"
                              "<fromUser>wxid_a</fromUser><pattedUser>wxid_b</pattedUser></record>"
                              "</records></patMsg></appmsg></msg>"),
                          kSelf, &d);
    Check(d.deliver, "a template-less pat delivers");
    Eq(d.content, "\"wxid_a\" 拍了拍 \"wxid_b\"", "template-less pat falls back to the plain wording");
    Check(satori::IsRevokeType(268445456) && satori::IsRevokeType(285222674) && !satori::IsRevokeType(10000),
          "revoke types");
    Check(satori::IsSystemType(10000) && satori::IsSystemType(268445456) && !satori::IsSystemType(1), "system types");
    satori::DecodeMessage(Row(306, 49, "wxid_f", "<msg><appmsg"), kSelf, &d);
    Check(d.deliver && Contains(d.content, "[消息]"), "malformed appmsg degrades to a placeholder");

    if (failures) {
        fprintf(stderr, "%d message test(s) failed\n", failures);
        return 1;
    }
    printf("message tests: PASS\n");
    return 0;
}
