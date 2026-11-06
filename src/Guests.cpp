// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include <algorithm>
#include <set>
namespace CnaService {
namespace {
bool guestId(const std::string& id) {return id.size()==34&&id.starts_with("g:")&&id.substr(2).find_first_not_of("0123456789abcdef")==std::string::npos;}
}
long long Service::guestNow() const {return guestClock_?guestClock_():now();}
std::string Service::guestParent(const std::string& participant) {
    if(!participant.starts_with("g:"))return participant;
    Statement q(store_.db(),"SELECT parent_id FROM guest_identities WHERE id=?");q.bind(1,participant);
    if(!q.row())throw Error("NOT_AUTHORIZED");
    return q.text(0);
}
void Service::pruneGuests() {
    // Only negotiated guest-mode memberships have a family lease. Legacy memberships
    // retain their original account/90-second directory lease semantics.
    Statement changed(store_.db(),"UPDATE directory_sessions SET revision=revision+1 WHERE guest_protocol=1 AND revision<2147483646 AND id IN(SELECT m.session_id FROM directory_members m LEFT JOIN refresh_families f ON f.id=m.family_id JOIN users u ON u.id=COALESCE(m.parent_id,m.user_id) WHERE m.family_id IS NOT NULL AND (f.id IS NULL OR f.revoked!=0 OR f.expires<=? OR u.online_allowed=0))");
    changed.bind(1,now());(void)changed.row();
    Statement owner(store_.db(),"UPDATE directory_machines SET expires=0 WHERE session_id IN(SELECT id FROM directory_sessions WHERE guest_protocol=1) AND NOT EXISTS(SELECT 1 FROM directory_members m JOIN refresh_families f ON f.id=m.family_id JOIN users u ON u.id=m.user_id WHERE m.machine_id=directory_machines.id AND m.user_id=directory_machines.owner_id AND f.revoked=0 AND f.expires>? AND u.online_allowed=1)");
    owner.bind(1,now());(void)owner.row();
    Statement invalid(store_.db(),"DELETE FROM directory_members WHERE family_id IS NOT NULL AND NOT EXISTS(SELECT 1 FROM refresh_families f JOIN users u ON u.id=f.user_id WHERE f.id=directory_members.family_id AND f.user_id=COALESCE(directory_members.parent_id,directory_members.user_id) AND f.game_id=directory_members.game_id AND f.revoked=0 AND f.expires>? AND u.online_allowed=1)");
    invalid.bind(1,now());(void)invalid.row();
    Statement identities(store_.db(),"DELETE FROM guest_identities WHERE NOT EXISTS(SELECT 1 FROM refresh_families f JOIN users u ON u.id=f.user_id WHERE f.id=guest_identities.family_id AND f.user_id=guest_identities.parent_id AND f.game_id=guest_identities.game_id AND f.revoked=0 AND f.expires>? AND u.online_allowed=1)");
    identities.bind(1,now());(void)identities.row();
    Statement budget(store_.db(),"DELETE FROM guest_open_times WHERE at<=?");budget.bind(1,guestNow()-60);(void)budget.row();
}
Json Service::guests(const std::string& actor,const std::string& game,const std::string& token,const std::string& op,const Json& args) {
    // Owner decision GAP-16: replacement service policy, not native Xbox credentials.
    const auto fam=participantAuthority(actor,game,Json{{"participants",Json::array({Json{{"access",token}}})}},true,true).front().family;
    pruneDirectory();
    if(op=="guests.close") {
        if(args.size()!=1||!args.contains("guest")||!args["guest"].is_string()||!guestId(args["guest"].get<std::string>()))throw Error("INVALID_ARGUMENT");
        Statement q(store_.db(),"SELECT 1 FROM guest_identities WHERE id=? AND parent_id=? AND family_id=? AND game_id=?");
        q.bind(1,args["guest"].get<std::string>());q.bind(2,actor);q.bind(3,fam);q.bind(4,game);if(!q.row())throw Error("NOT_AUTHORIZED");
        Statement rev(store_.db(),"UPDATE directory_sessions SET revision=revision+1 WHERE id IN(SELECT session_id FROM directory_members WHERE user_id=?) AND revision<2147483646");rev.bind(1,args["guest"].get<std::string>());(void)rev.row();
        Statement remove(store_.db(),"DELETE FROM guest_identities WHERE id=?");remove.bind(1,args["guest"].get<std::string>());(void)remove.row();
        fault("guest-after-close");return Json{{"closed",true}};
    }
    if(op!="guests.open")throw Error("UNKNOWN_OPERATION");
    if(args.size()!=1||!args.contains("slot"))throw Error("INVALID_ARGUMENT");
    const auto slot=integerField(args,"slot",1,3);
    Statement rate(store_.db(),"SELECT COUNT(*) FROM guest_open_times WHERE parent_id=? AND game_id=?");rate.bind(1,actor);rate.bind(2,game);(void)rate.row();
    if(rate.number(0)>=8)throw Error("RATE_LIMITED");
    Statement logs(store_.db(),"SELECT COUNT(*) FROM guest_open_times");(void)logs.row();if(logs.number(0)>=65536)throw Error("LIMIT_EXCEEDED");
    Statement tick(store_.db(),"INSERT INTO guest_open_times(parent_id,game_id,at) VALUES(?,?,?)");tick.bind(1,actor);tick.bind(2,game);tick.bind(3,guestNow());(void)tick.row();
    Statement prior(store_.db(),"SELECT id,gamertag FROM guest_identities WHERE parent_id=? AND family_id=? AND game_id=? AND slot=?");
    prior.bind(1,actor);prior.bind(2,fam);prior.bind(3,game);prior.bind(4,slot);
    std::string id,tag;
    if(prior.row()){id=prior.text(0);tag=prior.text(1);}else {
        Statement cap(store_.db(),"SELECT COUNT(*),COALESCE(SUM(game_id=?),0),COALESCE(SUM(game_id=? AND parent_id=?),0) FROM guest_identities");
        cap.bind(1,game);cap.bind(2,game);cap.bind(3,actor);(void)cap.row();
        if(cap.number(0)>=8192||cap.number(1)>=1024||cap.number(2)>=24)throw Error("LIMIT_EXCEEDED");
        Statement parent(store_.db(),"SELECT gamertag FROM users WHERE id=?");parent.bind(1,actor);if(!parent.row())throw Error("NOT_AUTHORIZED");
        tag=parent.text(0).substr(0,28)+" ("+std::to_string(slot)+")";id="g:"+randomHex(16);
        Statement insert(store_.db(),"INSERT INTO guest_identities(id,parent_id,family_id,game_id,slot,gamertag) VALUES(?,?,?,?,?,?)");
        insert.bind(1,id);insert.bind(2,actor);insert.bind(3,fam);insert.bind(4,game);insert.bind(5,slot);insert.bind(6,tag);(void)insert.row();
        fault("guest-after-open");
    }
    return Json{{"guestId",id},{"parentId",actor},{"gamertag",tag},{"slot",slot},{"allowOnlineSessions",true}};
}
std::vector<Service::ParticipantAuthority> Service::participantAuthority(const std::string& actor,const std::string& game,const Json& args,bool includesActor,bool guestAware) {
    if(!args.contains("participants")||!args["participants"].is_array()||args["participants"].empty()||args["participants"].size()>4)throw Error("INVALID_ARGUMENT");
    std::vector<ParticipantAuthority> result;std::set<std::string> seen;
    for(const auto& item:args["participants"]) {
        const bool guest=guestAware&&item.is_object()&&item.contains("guest");
        std::string token,id;
        if(guestAware) {
            if(!item.is_object()||item.size()!=(guest?2u:1u)||!item.contains(guest?"parentAccess":"access"))throw Error("INVALID_ARGUMENT");
            token=stringField(item,guest?"parentAccess":"access",64);
            if(guest){id=stringField(item,"guest",34);if(!guestId(id))throw Error("INVALID_ARGUMENT");}
        }else {if(!item.is_string())throw Error("INVALID_ARGUMENT");token=item.get<std::string>();}
        if(token.size()!=64||token.find_first_not_of("0123456789abcdef")!=std::string::npos)throw Error("INVALID_ARGUMENT");
        Statement auth(store_.db(),"SELECT s.user_id,f.id,f.expires FROM sessions s JOIN refresh_families f ON f.id=s.refresh_family JOIN users u ON u.id=s.user_id WHERE s.hash=? AND s.game_id=? AND s.expires>? AND f.user_id=s.user_id AND f.game_id=s.game_id AND f.revoked=0 AND f.expires>? AND u.online_allowed=1");
        auth.bind(1,sha256(token));auth.bind(2,game);auth.bind(3,now());auth.bind(4,now());if(!auth.row())throw Error("NOT_AUTHORIZED");
        ParticipantAuthority value{guest?id:auth.text(0),auth.text(0),auth.text(1),auth.number(2)};
        if(guest) {
            Statement g(store_.db(),"SELECT 1 FROM guest_identities WHERE id=? AND parent_id=? AND family_id=? AND game_id=?");
            g.bind(1,id);g.bind(2,value.parent);g.bind(3,value.family);g.bind(4,game);if(!g.row())throw Error("NOT_AUTHORIZED");
        }
        if(!seen.insert(value.participant).second)throw Error("INVALID_ARGUMENT");
        result.push_back(std::move(value));
    }
    if(seen.contains(actor)!=includesActor)throw Error(includesActor?"NOT_AUTHORIZED":"INVALID_ARGUMENT");
    if(guestAware)for(const auto& value:result)if(value.participant!=value.parent) {
        const auto parent=std::find_if(result.begin(),result.end(),[&](const auto& p){return p.participant==value.parent&&p.parent==p.participant&&p.family==value.family;});
        if(parent!=result.end())continue;
        if(includesActor||!args.contains("session"))throw Error("NOT_AUTHORIZED");
        Statement existing(store_.db(),"SELECT 1 FROM directory_members p JOIN directory_machines d ON d.id=p.machine_id WHERE p.user_id=? AND p.family_id=? AND p.session_id=? AND d.owner_id=? AND p.guest_id IS NULL AND d.expires>?");
        existing.bind(1,value.parent);existing.bind(2,value.family);existing.bind(3,stringField(args,"session",32));existing.bind(4,actor);existing.bind(5,now());if(!existing.row())throw Error("NOT_AUTHORIZED");
    }
    return result;
}
void Service::insertDirectoryMember(const std::string& session,const std::string& game,const std::string& participant,const std::string& machine,long long privateSlot,long long ordinal,const std::string& family) {
    Statement collision(store_.db(),"SELECT 1 FROM directory_members m LEFT JOIN users u ON u.id=m.account_id LEFT JOIN guest_identities g ON g.id=m.guest_id WHERE m.session_id=? AND COALESCE(g.gamertag,u.gamertag)=(SELECT gamertag FROM users WHERE id=? UNION ALL SELECT gamertag FROM guest_identities WHERE id=?)");
    collision.bind(1,session);collision.bind(2,participant);collision.bind(3,participant);if(collision.row())throw Error("INVALID_STATE");
    Statement insert(store_.db(),"INSERT INTO directory_members(session_id,game_id,user_id,machine_id,private_slot,ordinal,parent_id,family_id) VALUES(?,?,?,?,?,?,?,?)");
    insert.bind(1,session);insert.bind(2,game);insert.bind(3,participant);insert.bind(4,machine);insert.bind(5,privateSlot);insert.bind(6,ordinal);
    if(participant.starts_with("g:"))insert.bind(7,guestParent(participant));else insert.bindNull(7);
    if(family.empty())insert.bindNull(8);else insert.bind(8,family);(void)insert.row();fault("guest-after-member");
}
bool Service::guestVisible(const std::string& actor,const std::string& session) {
    Statement q(store_.db(),"SELECT 1 FROM directory_members m WHERE m.session_id=? AND m.account_id IS NOT NULL AND (EXISTS(SELECT 1 FROM player_reviews r WHERE r.reviewer_id=? AND r.subject_id=m.account_id AND r.rating='avoid') OR EXISTS(SELECT 1 FROM blocks b WHERE (b.user_id=? AND b.blocked_id=m.account_id) OR (b.user_id=m.account_id AND b.blocked_id=?)))");
    q.bind(1,session);q.bind(2,actor);q.bind(3,actor);q.bind(4,actor);return !q.row();
}
}
