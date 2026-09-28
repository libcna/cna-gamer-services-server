// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include <algorithm>
#include <set>

namespace CnaService {
namespace {
std::string sessionId(const Json& args) {
    const auto id=stringField(args,"session",32);
    if(id.size()!=32||id.find_first_not_of("0123456789abcdef")!=std::string::npos)throw Error("INVALID_ARGUMENT");
    return id;
}
bool booleanField(const Json& args,const char* key) {
    if(!args.contains(key)||!args[key].is_boolean())throw Error("INVALID_ARGUMENT");
    return args[key].get<bool>();
}
std::string kindField(const Json& args) {
    const auto kind=stringField(args,"kind",16);
    if(kind!="player"&&kind!="ranked")throw Error("INVALID_ARGUMENT");
    return kind;
}
class Transaction {
public:
    explicit Transaction(Store& store):store_(store){store_.exec("BEGIN IMMEDIATE");}
    ~Transaction(){if(!committed_)try{store_.exec("ROLLBACK");}catch(...){}}
    void commit(){store_.exec("COMMIT");committed_=true;}
private:
    Store& store_;bool committed_=false;
};
}
void Service::pruneDirectory() {
    Transaction transaction(store_);
    Statement closed(store_.db(),"DELETE FROM directory_sessions WHERE expires<=? OR revision>=2147483647 OR NOT EXISTS(SELECT 1 FROM directory_machines m WHERE m.id=host_machine AND m.expires>?)");
    closed.bind(1,now());closed.bind(2,now());(void)closed.row();
    Statement revision(store_.db(),"UPDATE directory_sessions SET revision=revision+1 WHERE id IN(SELECT session_id FROM directory_machines WHERE expires<=?)");revision.bind(1,now());(void)revision.row();
    Statement remove(store_.db(),"DELETE FROM directory_machines WHERE expires<=?");remove.bind(1,now());(void)remove.row();
    transaction.commit();
}
std::vector<std::string> Service::directoryParticipants(const std::string& user,const std::string& game,const Json& args) {
    if(!args.contains("participants")||!args["participants"].is_array()||args["participants"].empty()||args["participants"].size()>4)throw Error("INVALID_ARGUMENT");
    std::vector<std::string> participants;std::set<std::string> seen;
    for(const auto& credential:args["participants"]) {
        if(!credential.is_string()||credential.get_ref<const std::string&>().size()!=64)throw Error("INVALID_ARGUMENT");
        Statement person(store_.db(),"SELECT s.user_id,u.online_allowed FROM sessions s JOIN users u ON u.id=s.user_id WHERE s.hash=? AND s.game_id=? AND s.expires>?");
        person.bind(1,sha256(credential.get_ref<const std::string&>()));person.bind(2,game);person.bind(3,now());
        if(!person.row()||!person.number(1))throw Error("NOT_AUTHORIZED");
        if(!seen.insert(person.text(0)).second)throw Error("INVALID_ARGUMENT");
        participants.push_back(person.text(0));
    }
    if(!seen.contains(user))throw Error("NOT_AUTHORIZED");
    return participants;
}
Json Service::directorySnapshot(const std::string& id,bool includeMembers) {
    Statement session(store_.db(),"SELECT d.id,d.kind,d.state,d.max_gamers,d.private_slots,d.properties,d.allow_join,d.revision,d.host_id,u.gamertag,d.host_machine,(SELECT COUNT(*) FROM directory_members m WHERE m.session_id=d.id AND m.private_slot=0),(SELECT COUNT(*) FROM directory_members m WHERE m.session_id=d.id AND m.private_slot=1) FROM directory_sessions d JOIN users u ON u.id=d.host_id WHERE d.id=?");
    session.bind(1,id);if(!session.row())throw Error("NOT_FOUND");
    Json result{{"session",session.text(0)},{"kind",session.text(1)},{"state",session.text(2)},
        {"maxGamers",session.number(3)},{"privateSlots",session.number(4)},{"properties",parse(session.text(5))},
        {"allowJoinInProgress",session.number(6)!=0},{"revision",session.number(7)},
        {"hostId",session.text(8)},{"hostGamertag",session.text(9)},{"hostMachine",session.text(10)},
        {"currentGamers",session.number(11)+session.number(12)},
        {"openPublicSlots",session.number(3)-session.number(4)-session.number(11)},
        {"openPrivateSlots",session.number(4)-session.number(12)}};
    if(includeMembers) {
        Statement members(store_.db(),"SELECT m.user_id,u.gamertag,m.machine_id,m.private_slot,m.ordinal FROM directory_members m JOIN users u ON u.id=m.user_id WHERE m.session_id=? ORDER BY m.ordinal");
        members.bind(1,id);Json rows=Json::array();while(members.row())rows.push_back(Json{{"userId",members.text(0)},{"gamertag",members.text(1)},{"machine",members.text(2)},{"privateSlot",members.number(3)!=0},{"ordinal",members.number(4)}});
        result["members"]=std::move(rows);
    }
    return result;
}
Json Service::directory(const std::string& user,const std::string& game,const std::string& op,const Json& args) {
    static const std::map<std::string,std::set<std::string>> fields{
        {"sessions.create",{"kind","maxGamers","privateSlots","properties","allowJoinInProgress","participants"}},
        {"sessions.find",{"kind","localCount","properties","start","limit"}},
        {"sessions.join",{"session","participants"}},{"sessions.joinInvited",{"session","invite","participants"}},
        {"sessions.get",{"session"}},{"sessions.touch",{"session"}},{"sessions.leave",{"session"}},
        {"sessions.update",{"session","revision","maxGamers","privateSlots","properties","state","allowJoinInProgress"}}};
    const auto definition=fields.find(op);if(definition==fields.end())throw Error("UNKNOWN_OPERATION");
    if(args.size()!=definition->second.size())throw Error("INVALID_ARGUMENT");
    for(const auto& [name,value]:args.items()){(void)value;if(!definition->second.contains(name))throw Error("INVALID_ARGUMENT");}
    Statement allowed(store_.db(),"SELECT online_allowed FROM users WHERE id=?");allowed.bind(1,user);
    if(!allowed.row()||!allowed.number(0))throw Error("NOT_AUTHORIZED");
    pruneDirectory();
    if(op=="sessions.create") {
        const auto kind=kindField(args);const auto members=directoryParticipants(user,game,args);
        const auto maximum=integerField(args,"maxGamers",2,MaxSessionGamers),privateSlots=integerField(args,"privateSlots",0,maximum);
        if(members.size()>static_cast<std::size_t>(maximum))throw Error("INVALID_ARGUMENT");
        if(!args.contains("properties"))throw Error("INVALID_ARGUMENT");
        validateSessionProperties(args["properties"]);
        const bool allowJoin=booleanField(args,"allowJoinInProgress");
        if(kind=="ranked"&&allowJoin)throw Error("INVALID_ARGUMENT");
        const auto id=randomHex(16),machine=randomHex(16);Transaction transaction(store_);
        Statement cap(store_.db(),"SELECT COUNT(*),SUM(CASE WHEN host_id=? THEN 1 ELSE 0 END) FROM directory_sessions WHERE game_id=?");cap.bind(1,user);cap.bind(2,game);(void)cap.row();
        if(cap.number(0)>=1024||cap.number(1)>=16)throw Error("LIMIT_EXCEEDED");
        for(const auto& member:members) {
            Statement occupied(store_.db(),"SELECT 1 FROM directory_members WHERE game_id=? AND user_id=?");occupied.bind(1,game);occupied.bind(2,member);
            if(occupied.row())throw Error("INVALID_STATE");
        }
        Statement insert(store_.db(),"INSERT INTO directory_sessions(id,game_id,host_id,host_machine,kind,max_gamers,private_slots,properties,allow_join,created,expires) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
        insert.bind(1,id);insert.bind(2,game);insert.bind(3,user);insert.bind(4,machine);insert.bind(5,kind);insert.bind(6,maximum);insert.bind(7,privateSlots);insert.bind(8,args["properties"].dump());insert.bind(9,allowJoin?1LL:0LL);insert.bind(10,now());insert.bind(11,now()+90);(void)insert.row();
        Statement group(store_.db(),"INSERT INTO directory_machines(id,session_id,owner_id,expires) VALUES(?,?,?,?)");group.bind(1,machine);group.bind(2,id);group.bind(3,user);group.bind(4,now()+90);(void)group.row();
        for(std::size_t index=0;index<members.size();++index) {
            Statement join(store_.db(),"INSERT INTO directory_members(session_id,game_id,user_id,machine_id,private_slot,ordinal) VALUES(?,?,?,?,?,?)");
            join.bind(1,id);join.bind(2,game);join.bind(3,members[index]);join.bind(4,machine);join.bind(5,index>=static_cast<std::size_t>(maximum-privateSlots)?1LL:0LL);join.bind(6,static_cast<long long>(index));(void)join.row();
        }
        auto result=directorySnapshot(id,true);result["machine"]=machine;transaction.commit();return result;
    }
    if(op=="sessions.find") {
        const auto kind=kindField(args);const auto locals=integerField(args,"localCount",1,4),start=integerField(args,"start",0,1024),limit=integerField(args,"limit",1,32);
        if(!args.contains("properties"))throw Error("INVALID_ARGUMENT");
        validateSessionProperties(args["properties"]);
        Statement search(store_.db(),"SELECT d.id FROM directory_sessions d WHERE d.game_id=? AND d.kind=? AND (d.state='lobby' OR (d.kind='player' AND d.allow_join=1)) AND d.max_gamers-d.private_slots-(SELECT COUNT(*) FROM directory_members m WHERE m.session_id=d.id AND m.private_slot=0)>=? AND NOT EXISTS(SELECT 1 FROM directory_members m WHERE m.session_id=d.id AND m.user_id=?) AND NOT EXISTS(SELECT 1 FROM player_reviews r WHERE r.reviewer_id=?4 AND r.subject_id=d.host_id AND r.rating='avoid') AND NOT EXISTS(SELECT 1 FROM json_each(?) p WHERE p.value IS NOT NULL AND (json_extract(d.properties,'$['||p.key||']') IS NULL OR json_extract(d.properties,'$['||p.key||']')!=p.value)) ORDER BY d.created,d.id LIMIT ? OFFSET ?");
        search.bind(1,game);search.bind(2,kind);search.bind(3,locals);search.bind(4,user);search.bind(5,args["properties"].dump());search.bind(6,limit+1);search.bind(7,start);
        Json rows=Json::array();while(search.row())rows.push_back(directorySnapshot(search.text(0),false));
        const bool more=rows.size()>static_cast<std::size_t>(limit);if(more)rows.erase(rows.end()-1);
        return Json{{"sessions",rows},{"start",start},{"more",more}};
    }
    const auto id=sessionId(args);Transaction transaction(store_);
    Statement session(store_.db(),"SELECT host_id,host_machine,state,allow_join,revision,max_gamers,private_slots,kind FROM directory_sessions WHERE id=? AND game_id=?");session.bind(1,id);session.bind(2,game);
    if(!session.row())throw Error("NOT_FOUND");
    if(op=="sessions.join"||op=="sessions.joinInvited") {
        const bool invited=op=="sessions.joinInvited";
        const auto members=directoryParticipants(user,game,args);
        std::string inviteId,inviteStatus,usedMachine;
        if(invited) {
            inviteId=stringField(args,"invite",32);
            if(inviteId.size()!=32||inviteId.find_first_not_of("0123456789abcdef")!=std::string::npos)throw Error("INVALID_ARGUMENT");
            Statement invitation(store_.db(),"SELECT recipient_id,status,expires,used_machine FROM session_invitations WHERE id=? AND session_id=? AND game_id=?");
            invitation.bind(1,inviteId);invitation.bind(2,id);invitation.bind(3,game);
            if(!invitation.row())throw Error("NOT_FOUND");
            if(invitation.text(0)!=user)throw Error("NOT_AUTHORIZED");
            inviteStatus=invitation.text(1);usedMachine=invitation.text(3);
            if(invitation.number(2)<=now()||(inviteStatus!="accepted"&&inviteStatus!="used"))throw Error("INVALID_STATE");
        }
        auto consume=[&](const std::string& machine) {
            if(invited) {
                if(inviteStatus=="used"&&usedMachine!=machine)throw Error("INVALID_STATE");
                Statement update(store_.db(),"UPDATE session_invitations SET status='used',used_machine=? WHERE id=?");update.bind(1,machine);update.bind(2,inviteId);(void)update.row();
            }
        };
        Statement existing(store_.db(),"SELECT id FROM directory_machines WHERE session_id=? AND owner_id=?");existing.bind(1,id);existing.bind(2,user);
        if(existing.row()) {
            std::set<std::string> prior;Statement group(store_.db(),"SELECT user_id FROM directory_members WHERE machine_id=?");group.bind(1,existing.text(0));while(group.row())prior.insert(group.text(0));
            if(prior!=std::set<std::string>(members.begin(),members.end()))throw Error("INVALID_STATE");
            consume(existing.text(0));
            auto result=directorySnapshot(id,true);result["machine"]=existing.text(0);transaction.commit();return result;
        }
        if(invited&&inviteStatus=="used")throw Error("INVALID_STATE");
        if(session.text(2)=="playing"&&(session.text(7)=="ranked"||!session.number(3)))throw Error("INVALID_STATE");
        const auto snapshot=directorySnapshot(id,true);
        auto privateAvailable=invited?snapshot["openPrivateSlots"].get<std::size_t>():0;
        if(members.size()>snapshot["openPublicSlots"].get<std::size_t>()+privateAvailable)throw Error("SESSION_FULL");
        for(const auto& member:members) {
            Statement occupied(store_.db(),"SELECT 1 FROM directory_members WHERE game_id=? AND user_id=?");occupied.bind(1,game);occupied.bind(2,member);
            if(occupied.row())throw Error("INVALID_STATE");
        }
        std::set<long long> ordinals;for(const auto& member:snapshot["members"])ordinals.insert(member["ordinal"].get<long long>());
        const auto machine=randomHex(16);Statement group(store_.db(),"INSERT INTO directory_machines(id,session_id,owner_id,expires) VALUES(?,?,?,?)");group.bind(1,machine);group.bind(2,id);group.bind(3,user);group.bind(4,now()+90);(void)group.row();
        for(const auto& member:members) {
            long long ordinal=0;while(ordinals.contains(ordinal))++ordinal;ordinals.insert(ordinal);
            Statement join(store_.db(),"INSERT INTO directory_members(session_id,game_id,user_id,machine_id,private_slot,ordinal) VALUES(?,?,?,?,?,?)");
            join.bind(1,id);join.bind(2,game);join.bind(3,member);join.bind(4,machine);join.bind(5,privateAvailable?1LL:0LL);join.bind(6,ordinal);(void)join.row();
            if(privateAvailable)--privateAvailable;
        }
        Statement changed(store_.db(),"UPDATE directory_sessions SET revision=revision+1 WHERE id=?");changed.bind(1,id);(void)changed.row();
        consume(machine);
        auto result=directorySnapshot(id,true);result["machine"]=machine;transaction.commit();return result;
    }
    Statement member(store_.db(),"SELECT m.machine_id FROM directory_members m WHERE m.session_id=? AND m.user_id=?");member.bind(1,id);member.bind(2,user);
    if(!member.row())throw Error("NOT_AUTHORIZED");
    const auto machine=member.text(0);
    Statement owner(store_.db(),"SELECT owner_id FROM directory_machines WHERE id=?");owner.bind(1,machine);(void)owner.row();
    if(op=="sessions.get") {auto result=directorySnapshot(id,true);result["machine"]=machine;transaction.commit();return result;}
    if(op=="sessions.touch") {
        if(owner.text(0)!=user)throw Error("NOT_AUTHORIZED");
        Statement heartbeat(store_.db(),"UPDATE directory_machines SET expires=? WHERE id=?");heartbeat.bind(1,now()+90);heartbeat.bind(2,machine);(void)heartbeat.row();
        if(machine==session.text(1)){Statement host(store_.db(),"UPDATE directory_sessions SET expires=? WHERE id=?");host.bind(1,now()+90);host.bind(2,id);(void)host.row();}
        auto result=directorySnapshot(id,true);result["machine"]=machine;transaction.commit();return result;
    }
    if(op=="sessions.leave") {
        if(owner.text(0)!=user)throw Error("NOT_AUTHORIZED");
        const bool ended=machine==session.text(1);
        Statement remove(store_.db(),ended?"DELETE FROM directory_sessions WHERE id=?":"DELETE FROM directory_machines WHERE id=?");remove.bind(1,ended?id:machine);(void)remove.row();
        if(!ended){Statement changed(store_.db(),"UPDATE directory_sessions SET revision=revision+1 WHERE id=?");changed.bind(1,id);(void)changed.row();}
        transaction.commit();return Json{{"ended",ended}};
    }
    if(op=="sessions.update") {
        if(session.text(0)!=user||machine!=session.text(1))throw Error("NOT_AUTHORIZED");
        const auto revision=integerField(args,"revision",1,2147483646);
        if(revision!=session.number(4))throw Error("CONFLICT");
        const auto maximum=integerField(args,"maxGamers",2,MaxSessionGamers),privateSlots=integerField(args,"privateSlots",0,maximum);
        const auto state=stringField(args,"state",16);if(state!="lobby"&&state!="playing")throw Error("INVALID_STATE");
        const bool allowJoin=booleanField(args,"allowJoinInProgress");
        if(session.text(7)=="ranked"&&allowJoin)throw Error("INVALID_ARGUMENT");
        if(!args.contains("properties"))throw Error("INVALID_ARGUMENT");
        validateSessionProperties(args["properties"]);
        const auto current=directorySnapshot(id,false);
        if(privateSlots<current["privateSlots"].get<long long>()-current["openPrivateSlots"].get<long long>()||
            maximum-privateSlots<current["maxGamers"].get<long long>()-current["privateSlots"].get<long long>()-current["openPublicSlots"].get<long long>())throw Error("INVALID_ARGUMENT");
        Statement update(store_.db(),"UPDATE directory_sessions SET max_gamers=?,private_slots=?,properties=?,state=?,allow_join=?,revision=revision+1,expires=? WHERE id=?");
        update.bind(1,maximum);update.bind(2,privateSlots);update.bind(3,args["properties"].dump());update.bind(4,state);update.bind(5,allowJoin?1LL:0LL);update.bind(6,now()+90);update.bind(7,id);(void)update.row();
        if(session.text(7)=="ranked"&&session.text(2)!=state) {
            // Lobby->Playing opens an arbitration round over the exact current membership;
            // Playing->Lobby closes it so late or missing reports can be resolved.
            if(state=="playing") {
                Json members=Json::array();std::set<std::string> machines;
                Statement roster(store_.db(),"SELECT user_id,machine_id FROM directory_members WHERE session_id=? ORDER BY ordinal");roster.bind(1,id);
                while(roster.row()){members.push_back(Json{{"userId",roster.text(0)},{"machine",roster.text(1)}});machines.insert(roster.text(1));}
                Statement round(store_.db(),"INSERT INTO arbitration_rounds(id,game_id,session_id,start_revision,members,machines,created,updated) VALUES(?,?,?,?,?,?,?,?)");
                round.bind(1,randomHex(16));round.bind(2,game);round.bind(3,id);round.bind(4,revision+1);round.bind(5,members.dump());
                round.bind(6,Json(std::vector<std::string>(machines.begin(),machines.end())).dump());round.bind(7,now());round.bind(8,now());(void)round.row();
            }else {
                Statement close(store_.db(),"UPDATE arbitration_rounds SET end_revision=?,updated=? WHERE session_id=? AND end_revision IS NULL");
                close.bind(1,revision+1);close.bind(2,now());close.bind(3,id);(void)close.row();
            }
        }
        Statement heartbeat(store_.db(),"UPDATE directory_machines SET expires=? WHERE id=?");heartbeat.bind(1,now()+90);heartbeat.bind(2,machine);(void)heartbeat.row();
        auto result=directorySnapshot(id,true);result["machine"]=machine;transaction.commit();return result;
    }
    throw Error("UNKNOWN_OPERATION");
}
}
