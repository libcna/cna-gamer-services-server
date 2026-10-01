// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"

namespace CnaService {
namespace {
// An account may block this many others.
constexpr long long MaxBlocks=1024;
}
bool Service::friendsWith(const std::string& a,const std::string& b) {
    Statement mutual(store_.db(),"SELECT (SELECT COUNT(*) FROM friends WHERE (user_id=?1 AND friend_id=?2) OR (user_id=?2 AND friend_id=?1))=2");
    mutual.bind(1,a);mutual.bind(2,b);(void)mutual.row();return mutual.number(0)!=0;
}
// XNA AllowCommunication ("voice, text, messaging, or game invites") of both ends, and either
// end's block list. A friend request is how two players become friends, so a friends-only
// setting does not stop one; blocked communication and a block do.
void Service::mayCommunicate(const std::string& from,const std::string& to,bool friendRequest) {
    Statement blocked(store_.db(),"SELECT 1 FROM blocks WHERE (user_id=?1 AND blocked_id=?2) OR (user_id=?2 AND blocked_id=?1)");
    blocked.bind(1,from);blocked.bind(2,to);if(blocked.row())throw Error("NOT_AUTHORIZED");
    Statement settings(store_.db(),"SELECT privilege_communication FROM users WHERE id IN (?,?)");settings.bind(1,from);settings.bind(2,to);
    bool friendsOnly=false;
    while(settings.row()) {
        if(settings.text(0)=="blocked")throw Error("NOT_AUTHORIZED");
        friendsOnly=friendsOnly||settings.text(0)=="friends";
    }
    if(friendsOnly&&!friendRequest&&!friendsWith(from,to))throw Error("NOT_AUTHORIZED");
}
// XNA AllowProfileViewing of the viewer, and either end's block list. Everyone may read their own.
void Service::mayView(const std::string& viewer,const std::string& target) {
    if(viewer==target)return;
    Statement blocked(store_.db(),"SELECT 1 FROM blocks WHERE (user_id=?1 AND blocked_id=?2) OR (user_id=?2 AND blocked_id=?1)");
    blocked.bind(1,viewer);blocked.bind(2,target);if(blocked.row())throw Error("NOT_AUTHORIZED");
    Statement setting(store_.db(),"SELECT privilege_profile_viewing FROM users WHERE id=?");setting.bind(1,viewer);
    if(!setting.row()||setting.text(0)=="blocked")throw Error("NOT_AUTHORIZED");
    if(setting.text(0)=="friends"&&!friendsWith(viewer,target))throw Error("NOT_AUTHORIZED");
}
bool Service::mayReadAsset(const std::string& user,const std::string& game,const std::string& hash) {
    Statement shared(store_.db(),"SELECT 1 FROM title_assets WHERE game_id=? AND hash=? UNION "
        "SELECT 1 FROM avatar_catalog_assets WHERE hash=? LIMIT 1");
    shared.bind(1,game);shared.bind(2,hash);shared.bind(3,hash);
    if(shared.row())return true;
    Statement owners(store_.db(),"SELECT id FROM users WHERE picture=?");owners.bind(1,hash);
    bool picture=false;
    while(owners.row()) {
        picture=true;
        try {mayView(user,owners.text(0));return true;}
        catch(const Error& error) {if(error.code()!="NOT_AUTHORIZED")throw;}
    }
    // A hash shared by several accounts is readable through any viewable owner. Explicit title
    // and catalog assets above remain public to that title, independent of profile restrictions.
    if(picture)throw Error("NOT_AUTHORIZED");
    return false;
}
Json Service::privileges(const std::string& user) {
    Statement s(store_.db(),"SELECT privilege_communication,privilege_profile_viewing,privilege_user_content,privilege_trade,"
        "privilege_purchase,privilege_premium FROM users WHERE id=?");
    s.bind(1,user);if(!s.row())throw Error("NOT_FOUND");
    return Json{{"communication",s.text(0)},{"profileViewing",s.text(1)},{"userContent",s.text(2)},
        {"trade",s.number(3)!=0},{"purchase",s.number(4)!=0},{"premium",s.number(5)!=0}};
}
Json Service::privacy(const std::string& user,const std::string& op,const Json& args) {
    if(op=="privacy.list") {
        if(!args.empty())throw Error("INVALID_ARGUMENT");
        Statement rows(store_.db(),"SELECT u.gamertag FROM blocks b JOIN users u ON u.id=b.blocked_id WHERE b.user_id=? ORDER BY u.gamertag LIMIT ?");
        rows.bind(1,user);rows.bind(2,MaxBlocks);
        Json blocked=Json::array();while(rows.row())blocked.push_back(rows.text(0));
        return Json{{"blocked",blocked}};
    }
    if(args.size()!=1)throw Error("INVALID_ARGUMENT");
    Statement target(store_.db(),"SELECT id FROM users WHERE gamertag=?");target.bind(1,stringField(args,"gamertag",32));
    if(!target.row())throw Error("NOT_FOUND");
    const auto other=target.text(0);
    if(other==user)throw Error("INVALID_ARGUMENT");
    if(op=="privacy.unblock") {
        Statement remove(store_.db(),"DELETE FROM blocks WHERE user_id=? AND blocked_id=?");remove.bind(1,user);remove.bind(2,other);(void)remove.row();
        return Json::object();
    }
    if(op!="privacy.block")throw Error("UNKNOWN_OPERATION");
    Statement cap(store_.db(),"SELECT COUNT(*) FROM blocks WHERE user_id=?");cap.bind(1,user);(void)cap.row();
    Statement exists(store_.db(),"SELECT 1 FROM blocks WHERE user_id=? AND blocked_id=?");exists.bind(1,user);exists.bind(2,other);
    if(exists.row())return Json::object();
    if(cap.number(0)>=MaxBlocks)throw Error("LIMIT_EXCEEDED");
    Store::Transaction transaction(store_);
    Statement insert(store_.db(),"INSERT INTO blocks(user_id,blocked_id,created) VALUES(?,?,?)");
    insert.bind(1,user);insert.bind(2,other);insert.bind(3,now());(void)insert.row();
    // Blocking ends a friendship and anything pending between the two.
    Statement unfriend(store_.db(),"DELETE FROM friends WHERE (user_id=?1 AND friend_id=?2) OR (user_id=?2 AND friend_id=?1)");
    unfriend.bind(1,user);unfriend.bind(2,other);(void)unfriend.row();
    Statement invitations(store_.db(),"UPDATE session_invitations SET status='dismissed' WHERE status='pending' AND "
        "((sender_id=?1 AND recipient_id=?2) OR (sender_id=?2 AND recipient_id=?1))");
    invitations.bind(1,user);invitations.bind(2,other);(void)invitations.row();
    Statement parties(store_.db(),"DELETE FROM party_invitations WHERE (sender_id=?1 AND recipient_id=?2) OR (sender_id=?2 AND recipient_id=?1)");
    parties.bind(1,user);parties.bind(2,other);(void)parties.row();
    transaction.commit();
    hint(other,"friends");
    return Json::object();
}
}
