// SPDX-License-Identifier: MIT
// Parties: a small group of friends that stays together across titles (XNA SignedInGamer.PartySize,
// Guide.ShowParty and ShowPartySessions, LocalNetworkGamer.SendPartyInvites). One party per account;
// a party exists while it has members, and its leader passes to the longest-standing member.
#include "CnaService/Service.hpp"
#include <map>
#include <optional>
#include <set>

namespace CnaService {
namespace {
constexpr long long MaxPartySize=8;
constexpr long long PartyInvitationSeconds=3600;

std::string partyId(const Json& args) {
    const auto value=stringField(args,"party",32);
    if(value.size()!=32||value.find_first_not_of("0123456789abcdef")!=std::string::npos)throw Error("INVALID_ARGUMENT");
    return value;
}

class PartyTransaction {
public:
    explicit PartyTransaction(Store& store):store_(store){store_.exec("BEGIN IMMEDIATE");}
    ~PartyTransaction(){if(!committed_)try{store_.exec("ROLLBACK");}catch(...){}}
    void commit(){store_.exec("COMMIT");committed_=true;}
private:
    Store& store_;bool committed_=false;
};
}

Json Service::parties(const std::string& user,const std::string& game,const std::string& op,const Json& args) {
    static const std::map<std::string,std::set<std::string>> fields{
        {"parties.get",{}},{"parties.invite",{"gamertag"}},{"parties.accept",{"party"}},{"parties.decline",{"party"}},{"parties.leave",{}}};
    const auto definition=fields.find(op);if(definition==fields.end())throw Error("UNKNOWN_OPERATION");
    if(args.size()!=definition->second.size())throw Error("INVALID_ARGUMENT");
    for(const auto& [key,value]:args.items()){(void)value;if(!definition->second.contains(key))throw Error("INVALID_ARGUMENT");}
    const auto timestamp=now();
    PartyTransaction transaction(store_);
    Statement expire(store_.db(),"DELETE FROM party_invitations WHERE created<=?");expire.bind(1,timestamp-PartyInvitationSeconds);(void)expire.row();
    auto current=[&]()->std::optional<std::string> {
        Statement member(store_.db(),"SELECT party_id FROM party_members WHERE user_id=?");member.bind(1,user);
        if(!member.row())return std::nullopt;
        return member.text(0);
    };
    auto members=[&](const std::string& party) {
        Statement count(store_.db(),"SELECT COUNT(*) FROM party_members WHERE party_id=?");count.bind(1,party);(void)count.row();
        return count.number(0);
    };
    auto leave=[&](const std::string& party) {
        Statement remove(store_.db(),"DELETE FROM party_members WHERE party_id=? AND user_id=?");remove.bind(1,party);remove.bind(2,user);(void)remove.row();
        if(members(party)==0) {
            Statement end(store_.db(),"DELETE FROM parties WHERE id=?");end.bind(1,party);(void)end.row();
            return;
        }
        Statement lead(store_.db(),"UPDATE parties SET leader_id=(SELECT user_id FROM party_members WHERE party_id=?1 ORDER BY joined,user_id LIMIT 1) "
            "WHERE id=?1 AND leader_id=?2");
        lead.bind(1,party);lead.bind(2,user);(void)lead.row();
    };
    if(op=="parties.invite") {
        const auto gamertag=stringField(args,"gamertag",32);
        Statement target(store_.db(),"SELECT id FROM users WHERE gamertag=? COLLATE NOCASE");target.bind(1,gamertag);
        if(!target.row())throw Error("NOT_FOUND");
        const auto recipient=target.text(0);
        if(recipient==user)throw Error("INVALID_ARGUMENT");
        // Friends only: both have accepted each other.
        Statement friends(store_.db(),"SELECT EXISTS(SELECT 1 FROM friends WHERE user_id=?1 AND friend_id=?2) AND EXISTS(SELECT 1 FROM friends WHERE user_id=?2 AND friend_id=?1)");
        friends.bind(1,user);friends.bind(2,recipient);(void)friends.row();
        if(!friends.number(0))throw Error("NOT_AUTHORIZED");
        auto party=current();
        if(!party) {
            party=randomHex(16);
            Statement create(store_.db(),"INSERT INTO parties(id,leader_id,created) VALUES(?,?,?)");create.bind(1,*party);create.bind(2,user);create.bind(3,timestamp);(void)create.row();
            Statement join(store_.db(),"INSERT INTO party_members(party_id,user_id,joined) VALUES(?,?,?)");join.bind(1,*party);join.bind(2,user);join.bind(3,timestamp);(void)join.row();
        }
        Statement already(store_.db(),"SELECT 1 FROM party_members WHERE party_id=? AND user_id=?");already.bind(1,*party);already.bind(2,recipient);
        if(already.row())throw Error("CONFLICT");
        // Members and outstanding invitations together fit the party.
        Statement pending(store_.db(),"SELECT COUNT(*) FROM party_invitations WHERE party_id=? AND recipient_id<>?");pending.bind(1,*party);pending.bind(2,recipient);(void)pending.row();
        if(members(*party)+pending.number(0)>=MaxPartySize)throw Error("LIMIT_EXCEEDED");
        Statement invite(store_.db(),"INSERT INTO party_invitations(party_id,sender_id,recipient_id,created) VALUES(?,?,?,?) "
            "ON CONFLICT(party_id,recipient_id) DO UPDATE SET sender_id=excluded.sender_id,created=excluded.created");
        invite.bind(1,*party);invite.bind(2,user);invite.bind(3,recipient);invite.bind(4,timestamp);(void)invite.row();
    } else if(op=="parties.accept") {
        const auto party=partyId(args);
        Statement invitation(store_.db(),"SELECT 1 FROM party_invitations WHERE party_id=? AND recipient_id=?");invitation.bind(1,party);invitation.bind(2,user);
        if(!invitation.row())throw Error("NOT_FOUND");
        const auto previous=current();
        if(previous!=party) {
            if(members(party)>=MaxPartySize)throw Error("LIMIT_EXCEEDED");
            if(previous)leave(*previous);
            Statement join(store_.db(),"INSERT INTO party_members(party_id,user_id,joined) VALUES(?,?,?)");join.bind(1,party);join.bind(2,user);join.bind(3,timestamp);(void)join.row();
        }
        Statement used(store_.db(),"DELETE FROM party_invitations WHERE party_id=? AND recipient_id=?");used.bind(1,party);used.bind(2,user);(void)used.row();
    } else if(op=="parties.decline") {
        const auto party=partyId(args);
        Statement declined(store_.db(),"DELETE FROM party_invitations WHERE party_id=? AND recipient_id=?");declined.bind(1,party);declined.bind(2,user);(void)declined.row();
    } else if(op=="parties.leave") {
        if(const auto party=current())leave(*party);
    }
    // The caller's view: the party with each member's state in this title, and invitations.
    Json result{{"party",nullptr},{"invitations",Json::array()}};
    if(const auto party=current()) {
        Statement header(store_.db(),"SELECT leader_id FROM parties WHERE id=?");header.bind(1,*party);(void)header.row();
        Json list=Json::array();
        Statement rows(store_.db(),"SELECT u.id,u.gamertag,EXISTS(SELECT 1 FROM sessions z WHERE z.user_id=u.id AND z.expires>?1 AND z.last_seen>?2),"
            "COALESCE(p.mode,0),COALESCE(p.text,''),u.status,"
            "EXISTS(SELECT 1 FROM directory_members m JOIN directory_sessions d ON d.id=m.session_id WHERE m.game_id=?3 AND m.user_id=u.id "
            "AND d.kind='player' AND d.expires>?1 AND (d.state='lobby' OR d.allow_join=1) "
            "AND (SELECT COUNT(*) FROM directory_members o WHERE o.session_id=d.id AND o.private_slot=0)<d.max_gamers-d.private_slots) "
            "FROM party_members pm JOIN users u ON u.id=pm.user_id LEFT JOIN presence p ON p.user_id=u.id AND p.game_id=?3 "
            "WHERE pm.party_id=?4 ORDER BY pm.joined,u.id");
        rows.bind(1,timestamp);rows.bind(2,timestamp-90);rows.bind(3,game);rows.bind(4,*party);
        while(rows.row()) {
            const bool online=rows.number(2)!=0;
            list.push_back(Json{{"userId",rows.text(0)},{"gamertag",rows.text(1)},{"online",online},
                {"presenceMode",online?rows.number(3):0},{"presenceText",online?rows.text(4):""},
                {"away",online&&rows.text(5)=="away"},{"busy",online&&rows.text(5)=="busy"},{"joinable",online&&rows.number(6)!=0}});
        }
        result["party"]=Json{{"id",*party},{"leaderId",header.text(0)},{"members",std::move(list)}};
    }
    Statement inbox(store_.db(),"SELECT i.party_id,i.sender_id,u.gamertag,i.created,(SELECT COUNT(*) FROM party_members m WHERE m.party_id=i.party_id) "
        "FROM party_invitations i JOIN users u ON u.id=i.sender_id WHERE i.recipient_id=? ORDER BY i.created,i.party_id LIMIT 16");
    inbox.bind(1,user);
    while(inbox.row())
        result["invitations"].push_back(Json{{"party",inbox.text(0)},{"senderId",inbox.text(1)},{"senderGamertag",inbox.text(2)},
            {"created",inbox.number(3)},{"members",inbox.number(4)}});
    transaction.commit();
    return result;
}
}
