// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include <set>

namespace CnaService {
namespace {
std::string opaqueId(const Json& args,const char* field) {
    const auto value=stringField(args,field,32);
    if(value.size()!=32||value.find_first_not_of("0123456789abcdef")!=std::string::npos)
        throw Error("INVALID_ARGUMENT");
    return value;
}
}
Json Service::invitationSnapshot(const std::string& id) {
    Statement row(store_.db(),"SELECT i.id,i.session_id,i.sender_id,u.gamertag,i.status,i.created,i.expires,i.accepted_at,d.kind FROM session_invitations i JOIN users u ON u.id=i.sender_id JOIN directory_sessions d ON d.id=i.session_id WHERE i.id=?");
    row.bind(1,id);if(!row.row())throw Error("NOT_FOUND");
    return Json{{"invite",row.text(0)},{"session",row.text(1)},{"senderId",row.text(2)},
        {"senderGamertag",row.text(3)},{"status",row.text(4)},{"created",row.number(5)},
        {"expires",row.number(6)},{"acceptedAt",row.number(7)},{"kind",row.text(8)}};
}
Json Service::invitations(const std::string& user,const std::string& game,const std::string& op,const Json& args) {
    static const std::map<std::string,std::set<std::string>> fields{
        {"invites.send",{"session","gamertag"}},{"invites.list",{"start","limit"}},
        {"invites.get",{"invite"}},{"invites.accept",{"invite"}},{"invites.dismiss",{"invite"}},{"invites.joinFriend",{"gamertag"}}};
    const auto definition=fields.find(op);if(definition==fields.end())throw Error("UNKNOWN_OPERATION");
    if(args.size()!=definition->second.size())throw Error("INVALID_ARGUMENT");
    for(const auto& [key,value]:args.items()){(void)value;if(!definition->second.contains(key))throw Error("INVALID_ARGUMENT");}
    Statement privilege(store_.db(),"SELECT online_allowed FROM users WHERE id=?");privilege.bind(1,user);
    if(!privilege.row()||!privilege.number(0))throw Error("NOT_AUTHORIZED");
    pruneDirectory();
    const auto timestamp=now();
    Store::Transaction transaction(store_);
    Statement trim(store_.db(),"DELETE FROM session_invitations WHERE created<=?");trim.bind(1,timestamp-86400);(void)trim.row();
    if(op=="invites.send") {
        const auto session=opaqueId(args,"session"),gamertag=stringField(args,"gamertag",32);
        Statement listing(store_.db(),"SELECT 1 FROM directory_sessions WHERE id=? AND game_id=?");listing.bind(1,session);listing.bind(2,game);
        if(!listing.row())throw Error("NOT_FOUND");
        Statement member(store_.db(),"SELECT 1 FROM directory_members WHERE session_id=? AND user_id=?");member.bind(1,session);member.bind(2,user);
        if(!member.row())throw Error("NOT_AUTHORIZED");
        Statement target(store_.db(),"SELECT id,online_allowed FROM users WHERE gamertag=?");target.bind(1,gamertag);
        if(!target.row())throw Error("NOT_FOUND");
        const auto recipient=target.text(0);
        if(recipient==user)throw Error("INVALID_ARGUMENT");
        if(!target.number(1))throw Error("NOT_AUTHORIZED");
        mayCommunicate(user,recipient,false);
        Statement joined(store_.db(),"SELECT 1 FROM directory_members WHERE session_id=? AND user_id=?");joined.bind(1,session);joined.bind(2,recipient);
        if(joined.row())throw Error("INVALID_STATE");
        Statement duplicate(store_.db(),"SELECT id FROM session_invitations WHERE game_id=? AND session_id=? AND sender_id=? AND recipient_id=? AND status IN ('pending','accepted') AND expires>? ORDER BY created,id LIMIT 1");
        duplicate.bind(1,game);duplicate.bind(2,session);duplicate.bind(3,user);duplicate.bind(4,recipient);duplicate.bind(5,timestamp);
        if(duplicate.row()){auto result=invitationSnapshot(duplicate.text(0));transaction.commit();return result;}
        Statement titleCap(store_.db(),"SELECT COUNT(*) FROM session_invitations WHERE game_id=?");titleCap.bind(1,game);(void)titleCap.row();
        if(titleCap.number(0)>=MaxTitleInvites)throw Error("LIMIT_EXCEEDED");
        Statement incoming(store_.db(),"SELECT COUNT(*) FROM session_invitations WHERE game_id=? AND recipient_id=? AND status IN ('pending','accepted') AND expires>?");incoming.bind(1,game);incoming.bind(2,recipient);incoming.bind(3,timestamp);(void)incoming.row();
        if(incoming.number(0)>=MaxIncomingInvites)throw Error("LIMIT_EXCEEDED");
        Statement outgoing(store_.db(),"SELECT window_start,created_count FROM invitation_send_limits WHERE game_id=? AND user_id=?");outgoing.bind(1,game);outgoing.bind(2,user);
        long long window=timestamp,count=1;
        if(outgoing.row()&&timestamp-outgoing.number(0)<3600) {
            window=outgoing.number(0);count=outgoing.number(1)+1;
            if(count>MaxHourlyInvites)throw Error("RATE_LIMITED");
        }
        // Separate accounting survives session cascade/deletion; close/recreate cannot reset it.
        Statement quota(store_.db(),"INSERT INTO invitation_send_limits(game_id,user_id,window_start,created_count) VALUES(?,?,?,?) ON CONFLICT(game_id,user_id) DO UPDATE SET window_start=excluded.window_start,created_count=excluded.created_count");
        quota.bind(1,game);quota.bind(2,user);quota.bind(3,window);quota.bind(4,count);(void)quota.row();
        const auto invite=randomHex(16);
        Statement insert(store_.db(),"INSERT INTO session_invitations(id,game_id,session_id,sender_id,recipient_id,created,expires) VALUES(?,?,?,?,?,?,?)");
        insert.bind(1,invite);insert.bind(2,game);insert.bind(3,session);insert.bind(4,user);insert.bind(5,recipient);insert.bind(6,timestamp);insert.bind(7,timestamp+InviteLifetimeSeconds);(void)insert.row();
        auto result=invitationSnapshot(invite);transaction.commit();hint(recipient,"invitations");return result;
    }
    if(op=="invites.joinFriend") {
        // Joining a friend's or party member's game from the Guide: an invitation the friend's
        // session grants the asker, accepted by the asker like any other, never shown in an inbox.
        const auto gamertag=stringField(args,"gamertag",32);
        Statement target(store_.db(),"SELECT id FROM users WHERE gamertag=? COLLATE NOCASE");target.bind(1,gamertag);
        if(!target.row())throw Error("NOT_FOUND");
        const auto host=target.text(0);
        if(host==user)throw Error("INVALID_ARGUMENT");
        mayCommunicate(user,host,false);
        Statement related(store_.db(),"SELECT (EXISTS(SELECT 1 FROM friends WHERE user_id=?1 AND friend_id=?2) AND EXISTS(SELECT 1 FROM friends WHERE user_id=?2 AND friend_id=?1)) "
            "OR EXISTS(SELECT 1 FROM party_members a JOIN party_members b ON a.party_id=b.party_id WHERE a.user_id=?1 AND b.user_id=?2)");
        related.bind(1,user);related.bind(2,host);(void)related.row();
        if(!related.number(0))throw Error("NOT_AUTHORIZED");
        Statement session(store_.db(),"SELECT d.id FROM directory_members m JOIN directory_sessions d ON d.id=m.session_id WHERE m.game_id=?1 AND m.user_id=?2 "
            "AND d.kind='player' AND d.expires>?3 AND (d.state='lobby' OR d.allow_join=1) "
            "AND (SELECT COUNT(*) FROM directory_members o WHERE o.session_id=d.id AND o.private_slot=0)<d.max_gamers-d.private_slots LIMIT 1");
        session.bind(1,game);session.bind(2,host);session.bind(3,timestamp);
        if(!session.row())throw Error("NOT_FOUND");
        const auto id=session.text(0);
        Statement joined(store_.db(),"SELECT 1 FROM directory_members WHERE session_id=? AND user_id=?");joined.bind(1,id);joined.bind(2,user);
        if(joined.row())throw Error("INVALID_STATE");
        Statement existing(store_.db(),"SELECT id FROM session_invitations WHERE game_id=? AND session_id=? AND sender_id=? AND recipient_id=? AND requested=1 AND status='pending' AND expires>? LIMIT 1");
        existing.bind(1,game);existing.bind(2,id);existing.bind(3,host);existing.bind(4,user);existing.bind(5,timestamp);
        if(existing.row()){auto result=invitationSnapshot(existing.text(0));transaction.commit();return result;}
        Statement titleCap(store_.db(),"SELECT COUNT(*) FROM session_invitations WHERE game_id=?");titleCap.bind(1,game);(void)titleCap.row();
        if(titleCap.number(0)>=MaxTitleInvites)throw Error("LIMIT_EXCEEDED");
        const auto invite=randomHex(16);
        Statement insert(store_.db(),"INSERT INTO session_invitations(id,game_id,session_id,sender_id,recipient_id,created,expires,requested) VALUES(?,?,?,?,?,?,?,1)");
        insert.bind(1,invite);insert.bind(2,game);insert.bind(3,id);insert.bind(4,host);insert.bind(5,user);insert.bind(6,timestamp);insert.bind(7,timestamp+InviteLifetimeSeconds);(void)insert.row();
        auto result=invitationSnapshot(invite);transaction.commit();return result;
    }
    if(op=="invites.list") {
        const auto start=integerField(args,"start",0,MaxIncomingInvites),limit=integerField(args,"limit",1,32);
        Statement inbox(store_.db(),"SELECT id FROM session_invitations WHERE game_id=? AND recipient_id=? AND status IN ('pending','accepted') AND expires>? AND requested=0 ORDER BY created,id LIMIT ? OFFSET ?");
        inbox.bind(1,game);inbox.bind(2,user);inbox.bind(3,timestamp);inbox.bind(4,limit+1);inbox.bind(5,start);
        Json rows=Json::array();while(inbox.row())rows.push_back(invitationSnapshot(inbox.text(0)));
        const bool more=rows.size()>static_cast<std::size_t>(limit);if(more)rows.erase(rows.end()-1);
        transaction.commit();return Json{{"invites",rows},{"start",start},{"more",more}};
    }
    const auto invite=opaqueId(args,"invite");
    Statement row(store_.db(),"SELECT recipient_id,status,expires FROM session_invitations WHERE id=? AND game_id=?");row.bind(1,invite);row.bind(2,game);
    if(!row.row())throw Error("NOT_FOUND");
    if(row.text(0)!=user)throw Error("NOT_AUTHORIZED");
    if(row.number(2)<=timestamp)throw Error("INVALID_STATE");
    const auto status=row.text(1);
    if(op=="invites.accept") {
        if(status!="pending"&&status!="accepted")throw Error("INVALID_STATE");
        if(status=="pending") {
            Statement update(store_.db(),"UPDATE session_invitations SET status='accepted',accepted_at=? WHERE id=?");update.bind(1,timestamp);update.bind(2,invite);(void)update.row();
        }
    } else if(op=="invites.dismiss") {
        if(status=="used")throw Error("INVALID_STATE");
        if(status!="dismissed") {
            Statement update(store_.db(),"UPDATE session_invitations SET status='dismissed' WHERE id=?");update.bind(1,invite);(void)update.row();
        }
    }
    auto result=invitationSnapshot(invite);transaction.commit();return result;
}
}
