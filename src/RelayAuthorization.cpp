// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include "CnaService/RelayProtocol.hpp"
#include <algorithm>
#include <set>
namespace CnaService {
namespace {
bool hexId(const std::string& value,std::size_t size) {
    return value.size()==size&&value.find_first_not_of("0123456789abcdef")==std::string::npos;
}
class RelayTransaction {
public:
    explicit RelayTransaction(Store& store):store_(store){store_.exec("BEGIN IMMEDIATE");}
    ~RelayTransaction(){if(!committed_)try{store_.exec("ROLLBACK");}catch(...){}}
    void commit(){store_.exec("COMMIT");committed_=true;}
private:
    Store& store_;bool committed_=false;
};
}
void Service::pruneRelayTickets() {
    Statement remove(store_.db(),"DELETE FROM relay_tickets WHERE (used=0 AND expires<=?) OR grant_expires<=?");
    remove.bind(1,now());remove.bind(2,now());(void)remove.row();
}
Json Service::issueRelayTicket(const std::string& user,const std::string& game,const Json& args) {
    if(args.size()!=2||!args.contains("session")||!args.contains("participants"))throw Error("INVALID_ARGUMENT");
    const auto session=stringField(args,"session",32);if(!hexId(session,32))throw Error("INVALID_ARGUMENT");
    pruneDirectory();pruneRelayTickets();const auto users=directoryParticipants(user,game,args);
    Statement current(store_.db(),"SELECT id FROM directory_sessions WHERE id=? AND game_id=?");current.bind(1,session);current.bind(2,game);
    if(!current.row())throw Error("NOT_FOUND");
    Statement owner(store_.db(),"SELECT id FROM directory_machines WHERE session_id=? AND owner_id=?");owner.bind(1,session);owner.bind(2,user);
    if(!owner.row()) {
        Statement removed(store_.db(),"SELECT 1 FROM directory_removals WHERE session_id=? AND user_id=?");removed.bind(1,session);removed.bind(2,user);
        throw Error(removed.row()?"REMOVED_BY_HOST":"NOT_AUTHORIZED");
    }
    const auto machine=owner.text(0);
    std::set<std::string> roster;Statement members(store_.db(),"SELECT user_id FROM directory_members WHERE machine_id=?");members.bind(1,machine);
    while(members.row())roster.insert(members.text(0));
    if(roster!=std::set<std::string>(users.begin(),users.end()))throw Error("NOT_AUTHORIZED");
    struct Member {std::string user,family;};std::vector<Member> authority;long long deadline=now()+RelayGrantLifetimeSeconds;
    for(const auto& credential:args["participants"]) {
        Statement family(store_.db(),"SELECT s.user_id,f.id,f.expires FROM sessions s JOIN refresh_families f ON f.id=s.refresh_family WHERE s.hash=? AND s.game_id=? AND s.expires>? AND f.game_id=s.game_id AND f.user_id=s.user_id AND f.revoked=0 AND f.expires>?");
        family.bind(1,sha256(credential.get_ref<const std::string&>()));family.bind(2,game);family.bind(3,now());family.bind(4,now());
        if(!family.row())throw Error("NOT_AUTHORIZED");
        authority.push_back({family.text(0),family.text(1)});deadline=std::min(deadline,family.number(2));
    }
    RelayTransaction transaction(store_);
    Statement count(store_.db(),"SELECT COUNT(*) FROM relay_tickets WHERE machine_id=?");count.bind(1,machine);(void)count.row();
    if(count.number(0)>=MaxMachineRelayTickets)throw Error("LIMIT_EXCEEDED");
    Statement titleCount(store_.db(),"SELECT COUNT(*) FROM relay_tickets WHERE game_id=?");titleCount.bind(1,game);(void)titleCount.row();
    if(titleCount.number(0)>=MaxTitleRelayTickets)throw Error("LIMIT_EXCEEDED");
    const auto ticket=randomHex(32),hash=sha256(ticket);const auto timestamp=now(),expires=timestamp+RelayTicketLifetimeSeconds;
    Statement insert(store_.db(),"INSERT INTO relay_tickets(hash,game_id,session_id,machine_id,owner_id,participants_count,created,expires,grant_expires) VALUES(?,?,?,?,?,?,?,?,?)");
    insert.bind(1,hash);insert.bind(2,game);insert.bind(3,session);insert.bind(4,machine);insert.bind(5,user);
    insert.bind(6,static_cast<long long>(authority.size()));insert.bind(7,timestamp);insert.bind(8,expires);insert.bind(9,deadline);(void)insert.row();
    for(const auto& participant:authority) {
        Statement member(store_.db(),"INSERT INTO relay_ticket_members(ticket_hash,user_id,family_id) VALUES(?,?,?)");
        member.bind(1,hash);member.bind(2,participant.user);member.bind(3,participant.family);(void)member.row();
    }
    transaction.commit();
    return Json{{"ticket",ticket},{"session",session},{"machine",machine},{"expires",expires},{"serverTime",timestamp},
        {"relayVersion",RelayVersion},{"maxDatagramBytes",MaxRelayDatagramBytes}};
}
bool Service::validateRelayGrantLocked(const RelayGrant& grant,bool redeemed) {
    if(!identifier(grant.game)||!hexId(grant.ticketHash,64)||!hexId(grant.session,32)||!hexId(grant.machine,32)||!hexId(grant.owner,32))return false;
    Statement ticket(store_.db(),"SELECT participants_count,used,expires FROM relay_tickets WHERE hash=? AND game_id=? AND session_id=? AND machine_id=? AND owner_id=? AND grant_expires>?");
    ticket.bind(1,grant.ticketHash);ticket.bind(2,grant.game);ticket.bind(3,grant.session);ticket.bind(4,grant.machine);ticket.bind(5,grant.owner);ticket.bind(6,now());
    if(!ticket.row()||ticket.number(1)!=(redeemed?1:0)||(!redeemed&&ticket.number(2)<=now()))return false;
    const auto expected=ticket.number(0);if(expected<1||expected>4)return false;
    Statement machine(store_.db(),"SELECT 1 FROM directory_machines m JOIN directory_sessions d ON d.id=m.session_id WHERE m.id=? AND m.owner_id=? AND m.session_id=? AND d.game_id=? AND m.expires>? AND d.expires>?");
    machine.bind(1,grant.machine);machine.bind(2,grant.owner);machine.bind(3,grant.session);machine.bind(4,grant.game);machine.bind(5,now());machine.bind(6,now());
    if(!machine.row())return false;
    Statement roster(store_.db(),"SELECT COUNT(*) FROM directory_members WHERE machine_id=? AND session_id=? AND game_id=?");
    roster.bind(1,grant.machine);roster.bind(2,grant.session);roster.bind(3,grant.game);(void)roster.row();if(roster.number(0)!=expected)return false;
    Statement valid(store_.db(),"SELECT COUNT(*) FROM relay_ticket_members t JOIN refresh_families f ON f.id=t.family_id JOIN users u ON u.id=t.user_id JOIN directory_members m ON m.user_id=t.user_id AND m.machine_id=? AND m.session_id=? AND m.game_id=? WHERE t.ticket_hash=? AND f.user_id=t.user_id AND f.game_id=? AND f.revoked=0 AND f.expires>? AND u.online_allowed=1");
    valid.bind(1,grant.machine);valid.bind(2,grant.session);valid.bind(3,grant.game);valid.bind(4,grant.ticketHash);valid.bind(5,grant.game);valid.bind(6,now());
    (void)valid.row();return valid.number(0)==expected;
}
RelayGrant Service::redeemRelayTicket(const std::string& game,const std::string& ticket) {
    if(!identifier(game)||!hexId(ticket,64))throw Error("UNAUTHENTICATED");
    std::lock_guard lock(mutex_);pruneDirectory();pruneRelayTickets();RelayTransaction transaction(store_);
    const auto hash=sha256(ticket);Statement current(store_.db(),"SELECT session_id,machine_id,owner_id FROM relay_tickets WHERE hash=? AND game_id=?");
    current.bind(1,hash);current.bind(2,game);if(!current.row())throw Error("UNAUTHENTICATED");
    RelayGrant grant{hash,game,current.text(0),current.text(1),current.text(2)};
    if(!validateRelayGrantLocked(grant,false))throw Error("UNAUTHENTICATED");
    Statement used(store_.db(),"UPDATE relay_tickets SET used=1 WHERE hash=? AND used=0");used.bind(1,hash);(void)used.row();transaction.commit();return grant;
}
bool Service::validateRelayGrant(const RelayGrant& grant) {
    std::lock_guard lock(mutex_);pruneDirectory();pruneRelayTickets();return validateRelayGrantLocked(grant,true);
}
void Service::releaseRelayGrant(const RelayGrant& grant) {
    std::lock_guard lock(mutex_);
    Statement remove(store_.db(),"DELETE FROM relay_tickets WHERE hash=? AND game_id=? AND session_id=? AND machine_id=? AND owner_id=? AND used=1");
    remove.bind(1,grant.ticketHash);remove.bind(2,grant.game);remove.bind(3,grant.session);remove.bind(4,grant.machine);remove.bind(5,grant.owner);(void)remove.row();
}
}
