#include "wx_events.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace satori {
namespace {
constexpr int kRoomMax = 1024;
constexpr int kOutbox = 128;
constexpr int kRevokedSeen = 512;
// WeChat only lets a message be recalled for a short while, but a recall made while the phone was
// offline is synced later; look this far back for rewritten rows.
constexpr long long kRevokeWindowMs = 30LL * 60 * 1000;
constexpr int kFriendEvery = 4;  // friends are re-read every 4th pass
constexpr unsigned kRoomAuditEvery = 20;  // every 20th pass (about a minute) all rosters are re-read

struct Room {
    char name[80];
    long long modify_time, member_count, version;
    char *members;   // the snapshot everything is compared with
    char *pending;   // a different roster seen once; believed only if seen again
    bool alive;
};
} // namespace

struct Scanner {
    Store *store;
    int login_sn;
    bool baselined_revokes, baselined_rooms, baselined_friends;
    long long revoked_seen[kRevokedSeen];
    unsigned revoked_next;
    Room *rooms;
    int room_count;
    char *friends, *friends_pending;
    unsigned pass;
    char *outbox[kOutbox];
    int outbox_count;
    long long dropped;
};

namespace {
// ---- lists of ids -----------------------------------------------------------------------------
bool NextToken(const char **cursor, char separator, char *out, size_t capacity) {
    const char *p = *cursor;
    while (*p == separator) ++p;
    if (!*p) { *cursor = p; return false; }
    const char *end = strchr(p, separator);
    const size_t n = end ? static_cast<size_t>(end - p) : strlen(p);
    if (n >= capacity) { *cursor = end ? end + 1 : p + n; out[0] = 0; return true; }  // absurd id: skip it
    memcpy(out, p, n);
    out[n] = 0;
    *cursor = end ? end + 1 : p + n;
    return true;
}
bool Has(const char *list, char separator, const char *token) {
    if (!list || !token || !*token) return false;
    const size_t n = strlen(token);
    for (const char *p = list; *p;) {
        const char *end = strchr(p, separator);
        const size_t length = end ? static_cast<size_t>(end - p) : strlen(p);
        if (length == n && !memcmp(p, token, n)) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}
bool SameSet(const char *a, const char *b, char separator) {
    char token[96];
    for (const char *p = a; NextToken(&p, separator, token, sizeof(token));) if (*token && !Has(b, separator, token)) return false;
    for (const char *p = b; NextToken(&p, separator, token, sizeof(token));) if (*token && !Has(a, separator, token)) return false;
    return true;
}

// ---- events ---------------------------------------------------------------------------------
cJSON *BaseEvent(const Scanner &scanner, const char *type, long long now_ms) {
    cJSON *event = cJSON_CreateObject();
    cJSON *login = cJSON_CreateObject();
    if (!event || !login) { cJSON_Delete(event); cJSON_Delete(login); return nullptr; }
    cJSON_AddNumberToObject(login, "sn", scanner.login_sn);
    cJSON_AddItemToObject(event, "login", login);
    cJSON_AddStringToObject(event, "type", type);
    cJSON_AddNumberToObject(event, "timestamp", static_cast<double>(now_ms));
    return event;
}

// Takes ownership of `event`. A full retry queue drops the newest and counts it: the bus being
// stuck for minutes is a bigger problem than one missing notification.
void Enqueue(Scanner *scanner, cJSON *event) {
    if (!event) return;
    char *text = cJSON_PrintUnformatted(event);
    cJSON_Delete(event);
    if (!text) return;
    if (scanner->outbox_count >= kOutbox) { free(text); ++scanner->dropped; return; }
    scanner->outbox[scanner->outbox_count++] = text;
}

void Flush(Scanner *scanner, bool (*emit)(void *, const char *), void *context) {
    int sent = 0;
    while (sent < scanner->outbox_count && emit(context, scanner->outbox[sent])) { free(scanner->outbox[sent]); ++sent; }
    if (!sent) return;
    memmove(scanner->outbox, scanner->outbox + sent, static_cast<size_t>(scanner->outbox_count - sent) * sizeof(char *));
    scanner->outbox_count -= sent;
}

void AddChannel(Store *store, cJSON *event, const char *talker) {
    const bool group = strstr(talker, "@chatroom") != nullptr;
    cJSON *channel = cJSON_CreateObject();
    if (channel) {
        cJSON_AddStringToObject(channel, "id", talker);
        cJSON_AddNumberToObject(channel, "type", group ? 0 : 1);
        cJSON_AddItemToObject(event, "channel", channel);
    }
    if (group) cJSON_AddItemToObject(event, "guild", StoreGuildObject(store, talker));
}

// A message that was there and now is not. `user` is only as good as our memory of who wrote it.
void AnnounceRevoked(Scanner *scanner, const RevokedRow &row, long long now_ms) {
    cJSON *event = BaseEvent(*scanner, "message-deleted", now_ms);
    if (!event) return;
    AddChannel(scanner->store, event, row.talker);
    char author[96];
    bool known = StoreAuthorOf(scanner->store, row.id, author, sizeof(author));
    // In a private chat the only other person is the talker; in a group there is no such guess.
    if (!known && !strstr(row.talker, "@chatroom")) { snprintf(author, sizeof(author), "%s", row.talker); known = true; }
    cJSON *message = cJSON_CreateObject();
    if (message) {
        char id[24];
        snprintf(id, sizeof(id), "%lld", row.id);
        cJSON_AddStringToObject(message, "id", id);
        cJSON_AddNumberToObject(message, "timestamp", static_cast<double>(row.create_time));
        cJSON_AddItemToObject(event, "message", message);
    }
    if (known) {
        cJSON_AddItemToObject(event, "user", StoreUserObject(scanner->store, author));
        if (message) cJSON_AddItemToObject(message, "user", StoreUserObject(scanner->store, author));
    }
    Enqueue(scanner, event);
}

void AnnounceGuild(Scanner *scanner, const char *type, const char *room, long long now_ms) {
    cJSON *event = BaseEvent(*scanner, type, now_ms);
    if (!event) return;
    cJSON_AddItemToObject(event, "guild", StoreGuildObject(scanner->store, room));
    Enqueue(scanner, event);
}

void AnnounceMember(Scanner *scanner, const char *type, const char *room, const char *user_id, long long now_ms) {
    cJSON *event = BaseEvent(*scanner, type, now_ms);
    if (!event) return;
    cJSON_AddItemToObject(event, "guild", StoreGuildObject(scanner->store, room));
    cJSON *user = StoreUserObject(scanner->store, user_id);
    cJSON *member = cJSON_CreateObject();
    if (member && user) cJSON_AddItemToObject(member, "user", cJSON_Duplicate(user, true));
    if (user) cJSON_AddItemToObject(event, "user", user);
    if (member) cJSON_AddItemToObject(event, "member", member);
    Enqueue(scanner, event);
}

void AnnounceFriend(Scanner *scanner, const char *type, const char *user_id, long long now_ms) {
    cJSON *event = BaseEvent(*scanner, type, now_ms);
    if (!event) return;
    cJSON *user = StoreUserObject(scanner->store, user_id);
    cJSON *friend_object = cJSON_CreateObject();
    if (friend_object && user) cJSON_AddItemToObject(friend_object, "user", cJSON_Duplicate(user, true));
    if (user) cJSON_AddItemToObject(event, "user", user);
    if (friend_object) cJSON_AddItemToObject(event, "friend", friend_object);
    Enqueue(scanner, event);
}

// ---- recalled messages -------------------------------------------------------------------------
bool SeenRevoked(const Scanner &scanner, long long id) {
    for (int i = 0; i < kRevokedSeen; ++i) if (scanner.revoked_seen[i] == id) return true;
    return false;
}

void ScanRevoked(Scanner *scanner, long long now_ms) {
    RevokedRow rows[128];
    const int count = StoreRevoked(scanner->store, now_ms - kRevokeWindowMs, rows, 128);
    if (count < 0) return;  // unreadable this time: try again, and do not baseline on a failure
    for (int i = 0; i < count; ++i) {
        if (SeenRevoked(*scanner, rows[i].id)) continue;
        scanner->revoked_seen[scanner->revoked_next++ % kRevokedSeen] = rows[i].id;
        if (scanner->baselined_revokes) AnnounceRevoked(scanner, rows[i], now_ms);
    }
    scanner->baselined_revokes = true;
}

// ---- group membership ----------------------------------------------------------------------------
Room *FindRoom(Scanner *scanner, const char *name) {
    for (int i = 0; i < scanner->room_count; ++i) if (!strcmp(scanner->rooms[i].name, name)) return &scanner->rooms[i];
    return nullptr;
}

void DropRoom(Scanner *scanner, int index) {
    free(scanner->rooms[index].members);
    free(scanner->rooms[index].pending);
    scanner->rooms[index] = scanner->rooms[--scanner->room_count];
}

void AnnounceRosterChange(Scanner *scanner, const char *room, const char *before, const char *after, long long now_ms) {
    const char *self = StoreSelfId(scanner->store);
    char token[96];
    for (const char *p = after; NextToken(&p, ';', token, sizeof(token));) {
        if (!*token || Has(before, ';', token)) continue;
        if (!strcmp(token, self)) AnnounceGuild(scanner, "guild-added", room, now_ms);
        else AnnounceMember(scanner, "guild-member-added", room, token, now_ms);
    }
    for (const char *p = before; NextToken(&p, ';', token, sizeof(token));) {
        if (!*token || Has(after, ';', token)) continue;
        if (!strcmp(token, self)) AnnounceGuild(scanner, "guild-removed", room, now_ms);
        else AnnounceMember(scanner, "guild-member-removed", room, token, now_ms);
    }
}

void ScanRooms(Scanner *scanner, long long now_ms) {
    static RoomStamp stamps[kRoomMax];
    const bool audit = scanner->pass % kRoomAuditEvery == kRoomAuditEvery - 1;
    const int count = StoreRoomStamps(scanner->store, stamps, kRoomMax);
    if (count < 0) return;
    const char *self = StoreSelfId(scanner->store);
    for (int i = 0; i < scanner->room_count; ++i) scanner->rooms[i].alive = false;
    for (int i = 0; i < count; ++i) {
        Room *room = FindRoom(scanner, stamps[i].name);
        if (!room) {
            if (scanner->room_count >= kRoomMax) continue;
            char *members = StoreRoomMembers(scanner->store, stamps[i].name);
            if (!members) continue;
            Room &fresh = scanner->rooms[scanner->room_count++];
            fresh = Room{};
            snprintf(fresh.name, sizeof(fresh.name), "%s", stamps[i].name);
            fresh.modify_time = stamps[i].modify_time; fresh.member_count = stamps[i].member_count; fresh.version = stamps[i].version;
            fresh.members = members;
            fresh.alive = true;
            // A room that appears after the baseline and lists us is one we just joined.
            if (scanner->baselined_rooms && Has(members, ';', self)) AnnounceGuild(scanner, "guild-added", stamps[i].name, now_ms);
            continue;
        }
        room->alive = true;
        // Every so often re-read a roster whatever its fingerprint says, in case a change never
        // touched the columns the fingerprint is made of.
        const bool unchanged = !audit && room->modify_time == stamps[i].modify_time && room->member_count == stamps[i].member_count &&
                               room->version == stamps[i].version;
        if (unchanged) { free(room->pending); room->pending = nullptr; continue; }
        char *members = StoreRoomMembers(scanner->store, stamps[i].name);
        if (!members) continue;
        if (SameSet(members, room->members, ';')) {
            // Only metadata (a nickname, the notice) changed; keep the fresh order and stamp.
            free(room->members); room->members = members;
            free(room->pending); room->pending = nullptr;
            room->modify_time = stamps[i].modify_time; room->member_count = stamps[i].member_count; room->version = stamps[i].version;
        } else if (room->pending && SameSet(room->pending, members, ';')) {
            // Seen twice in a row: the roster really changed.
            AnnounceRosterChange(scanner, stamps[i].name, room->members, members, now_ms);
            free(room->members); free(room->pending);
            room->members = members; room->pending = nullptr;
            room->modify_time = stamps[i].modify_time; room->member_count = stamps[i].member_count; room->version = stamps[i].version;
        } else {
            free(room->pending);
            room->pending = members;
        }
    }
    // A room that has vanished is only news when we were in it, and only when the read was
    // complete (a full buffer means some rooms were not looked at).
    if (count < kRoomMax) {
        for (int i = scanner->room_count - 1; i >= 0; --i) {
            if (scanner->rooms[i].alive) continue;
            if (scanner->baselined_rooms && Has(scanner->rooms[i].members, ';', self)) AnnounceGuild(scanner, "guild-removed", scanner->rooms[i].name, now_ms);
            DropRoom(scanner, i);
        }
    }
    scanner->baselined_rooms = true;
}

// ---- friends -----------------------------------------------------------------------------------------
void ScanFriends(Scanner *scanner, long long now_ms) {
    char *current = StoreFriendIds(scanner->store);
    if (!current) return;
    if (!scanner->friends) { scanner->friends = current; scanner->baselined_friends = true; return; }
    if (SameSet(current, scanner->friends, '\n')) {
        free(scanner->friends); scanner->friends = current;
        free(scanner->friends_pending); scanner->friends_pending = nullptr;
        return;
    }
    if (scanner->friends_pending && SameSet(scanner->friends_pending, current, '\n')) {
        char token[96];
        for (const char *p = current; NextToken(&p, '\n', token, sizeof(token));)
            if (*token && !Has(scanner->friends, '\n', token)) AnnounceFriend(scanner, "friend-added", token, now_ms);
        for (const char *p = scanner->friends; NextToken(&p, '\n', token, sizeof(token));)
            if (*token && !Has(current, '\n', token)) AnnounceFriend(scanner, "friend-removed", token, now_ms);
        free(scanner->friends); free(scanner->friends_pending);
        scanner->friends = current; scanner->friends_pending = nullptr;
        return;
    }
    free(scanner->friends_pending);
    scanner->friends_pending = current;
}
} // namespace

Scanner *CreateScanner(Store *store, int login_sn) {
    if (!store) return nullptr;
    auto *scanner = static_cast<Scanner *>(calloc(1, sizeof(Scanner)));
    if (!scanner) return nullptr;
    scanner->rooms = static_cast<Room *>(calloc(kRoomMax, sizeof(Room)));
    if (!scanner->rooms) { free(scanner); return nullptr; }
    scanner->store = store;
    scanner->login_sn = login_sn;
    return scanner;
}

void DestroyScanner(Scanner *scanner) {
    if (!scanner) return;
    for (int i = 0; i < scanner->room_count; ++i) { free(scanner->rooms[i].members); free(scanner->rooms[i].pending); }
    for (int i = 0; i < scanner->outbox_count; ++i) free(scanner->outbox[i]);
    free(scanner->rooms); free(scanner->friends); free(scanner->friends_pending);
    free(scanner);
}

void ScannerStep(Scanner *scanner, bool (*emit)(void *, const char *), void *context, long long now_ms) {
    if (!scanner || !emit) return;
    Flush(scanner, emit, context);  // earlier events first, so order is never swapped by a retry
    ScanRevoked(scanner, now_ms);
    ScanRooms(scanner, now_ms);
    if (scanner->pass++ % kFriendEvery == 0) ScanFriends(scanner, now_ms);
    Flush(scanner, emit, context);
}

long long ScannerDropped(Scanner *scanner) { return scanner ? scanner->dropped : 0; }
} // namespace satori
