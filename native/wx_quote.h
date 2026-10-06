#pragma once
// What a quote-reply needs to know about the message it replies to. The store fills it from the
// message table (StoreQuoteTarget), the backend adds the display name, the sender (SendQuote)
// turns it into WeChat's own <refermsg>.
namespace satori {
struct QuoteRef {
    long long local_id;  // the quoted row's msgId
    long long svr_id;    // its server id: what the recipient's client finds it by
    long long created_s; // when it was sent, in seconds
    int row_type;        // the row's `type` in WeChat's table: 1 is a text message
    char talker[96];     // the conversation
    char sender[96];     // who wrote it (the group member in a group, else the peer or ourselves)
    char display[160];   // the sender's name as the conversation shows it
    char text[4096];     // the quoted text, or "[图片]"-style for anything that is not text
};
} // namespace satori
