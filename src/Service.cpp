// SPDX-License-Identifier: MIT
#include "CnaService/Service.hpp"
#include <algorithm>
#include <chrono>
#include <openssl/crypto.h>
#include <cmath>
#include <optional>
#include <set>
#include <utility>

namespace CnaService {
namespace {
// Leases, presence, relay tickets and the per-request bookkeeping are rebuilt within seconds of
// a crash, so they commit without waiting for the disk. Everything a player would miss after a
// power loss -- credentials and revocations, awards, scores, friends, messages, profile and
// avatar edits -- commits with a full sync.
const std::set<std::string> Ephemeral{"auth.ping","gamer.lookup","profile.get","profile.gameDefaults","friends.list",
    "presence.set","achievements.list","assets.read","leaderboards.read","leaderboards.definition","leaderboards.list","parties.get","sessions.relayTicket",
    "sessions.create","sessions.find","sessions.get","sessions.touch","sessions.update","sessions.join","sessions.joinInvited",
    "sessions.leave","sessions.remove","sessions.addMembers","invites.list","invites.get","messages.list","avatars.get","avatars.catalog","avatars.catalogPack","privacy.list"};
// Replay protection is for requests whose repetition would change something twice. Reads, and
// writes that replace a value outright and recur on a timer (heartbeat, lease, presence), repeat
// harmlessly; recording them would spend a title's daily budget on its own keep-alive traffic.
const std::set<std::string> Unrecorded{"auth.ping","sessions.touch","presence.set","presence.status","gamer.lookup",
    "profile.get","profile.gameDefaults","friends.list","achievements.list","assets.read","leaderboards.read",
    "leaderboards.definition","leaderboards.list","parties.get","sessions.find","sessions.get","invites.list","invites.get","messages.list",
    "avatars.get","avatars.catalog","avatars.catalogPack","privacy.list"};
// Recorded request IDs per title and 24 hours: the storage backstop.
constexpr long long MaxTitleRequestIds=1000000;
// Recorded request IDs per account, title and 24-hour window, so that no one account can spend
// the title's budget for everyone else.
constexpr int MaxAccountRequestIds=20000;
// The largest result kept for answering a repeat; a larger one is refused as a duplicate. Every
// result of a recorded operation is far smaller (a session snapshot of 31 gamers is about 6 KiB).
constexpr std::size_t MaxStoredResultBytes=16384;
class DurableScope {
public:
    DurableScope(Store& store,bool durable):store_(durable?&store:nullptr) {if(store_)store_->exec("PRAGMA synchronous=FULL");}
    ~DurableScope() {if(store_)try{store_->exec("PRAGMA synchronous=NORMAL");}catch(...){}}
    DurableScope(const DurableScope&)=delete;
    DurableScope& operator=(const DurableScope&)=delete;
private:
    Store* store_;
};
}
Service::FaultHook Service::faultHook_=nullptr;
void Service::setFaultHookForTesting(FaultHook hook) {faultHook_=hook;}
void Service::fault(const char* point) {if(faultHook_)faultHook_(point);}
Service::Service(const std::string& database):store_(database) {store_.exec("PRAGMA synchronous=NORMAL");}
std::string Service::handle(std::string_view bytes,std::string_view peer) {
    const auto metricStarted=std::chrono::steady_clock::now();
    std::string id,code="OK",result,operation="invalid";
    std::vector<std::pair<std::string,std::string>> hints;
    std::function<void(const std::string&,const std::string&)> sink;
    try {
        const auto request=parse(bytes);validateRequest(request);id=stringField(request,"id",64);
        operation=stringField(request,"op",64);
        std::unique_lock lock(mutex_);
        hints_.clear();
        result=response(id,"OK",dispatch(request,std::string(peer),lock)).dump();
        if (result.size()>MaxMessageBytes) throw Error("LIMIT_EXCEEDED");
        // Only a request that succeeded tells anyone; the sink runs after the lock is released.
        hints.swap(hints_);sink=hintSink_;
    } catch (const Error& e) { code=e.code();result=response(id,code).dump(); }
      catch (...) { code="INTERNAL_ERROR";result=response(id,code).dump(); }
    if(sink)for(const auto& [user,topic]:hints){try{sink(user,topic);}catch(...){}}
    if(code=="UNKNOWN_OPERATION")operation="unknown";
    const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()-metricStarted).count();
    constexpr std::array<long long,7> LatencyBounds{1000,5000,10000,25000,50000,100000,250000};
    std::lock_guard counting(outcomesMutex_);
    ++outcomes_[code];++metricOutcomes_[code];++metricOperations_[operation];
    for(std::size_t index=0;index<LatencyBounds.size();++index)
        if(elapsed<=LatencyBounds[index])++requestLatencyBuckets_[index];
    ++requestLatencyBuckets_.back();requestLatencyMicroseconds_+=static_cast<unsigned long long>(elapsed);
    return result;
}
void Service::setHintSink(std::function<void(const std::string& user,const std::string& topic)> sink) {
    std::lock_guard lock(mutex_);hintSink_=std::move(sink);
}
void Service::hint(const std::string& user,const char* topic) {
    fault("hint");
    if(hints_.size()<256&&std::find(hints_.begin(),hints_.end(),std::pair<std::string,std::string>{user,topic})==hints_.end())
        hints_.emplace_back(user,topic);
}
std::string Service::eventAccount(std::string_view game,std::string_view token) {
    if(!identifier(std::string(game))||token.size()!=64)throw Error("UNAUTHENTICATED");
    std::unique_lock lock(mutex_);
    Statement session(store_.db(),"SELECT user_id FROM sessions WHERE hash=? AND game_id=? AND expires>?");
    session.bind(1,sha256(token));session.bind(2,std::string(game));session.bind(3,now());
    if(!session.row())throw Error("UNAUTHENTICATED");
    return session.text(0);
}
Service::File Service::file(std::string_view game,std::string_view token,std::string_view hash) {
    const auto metricStarted=std::chrono::steady_clock::now();
    // One account may download this much an hour: dozens of complete catalogs, far below what a
    // client that re-downloads in a loop would take.
    constexpr long long MaxDownloadBytesPerHour=1LL<<30;
    File out;
    try {
        if(!identifier(std::string(game))||token.size()!=64||hash.size()!=64||hash.find_first_not_of("0123456789abcdef")!=std::string_view::npos)
            throw Error("INVALID_ARGUMENT");
        std::unique_lock lock(mutex_);
        const auto timestamp=now();
        Statement session(store_.db(),"SELECT user_id FROM sessions WHERE hash=? AND game_id=? AND expires>?");
        session.bind(1,sha256(token));session.bind(2,game);session.bind(3,timestamp);
        if(!session.row())throw Error("UNAUTHENTICATED");
        const auto user=session.text(0);
        const bool authorized=mayReadAsset(user,std::string(game),std::string(hash));
        if(authorized) {
            Statement asset(store_.db(),"SELECT mime,bytes FROM assets WHERE hash=?");asset.bind(1,hash);
            if(!asset.row())throw Error("NOT_FOUND");
            out.mime=asset.text(0);out.bytes=asset.blob(1);
        } else {
            Statement versions(store_.db(),"SELECT version FROM avatar_catalogs");
            while(out.bytes.empty()&&versions.row()) {
                const auto& info=catalog(versions.number(0));
                if(info.manifestSha256==hash){out.mime="application/json";out.bytes=info.manifest;}
            }
            if(out.bytes.empty())throw Error("NOT_FOUND");
        }
        auto& budget=downloads_[std::string(game)+'\n'+user];
        if(timestamp-budget.start>=3600)budget={timestamp,0};
        if(budget.bytes+static_cast<long long>(out.bytes.size())>MaxDownloadBytesPerHour)throw Error("RATE_LIMITED");
        budget.bytes+=static_cast<long long>(out.bytes.size());
        if(downloads_.size()>65536)std::erase_if(downloads_,[&](const auto& entry){return timestamp-entry.second.start>=3600;});
        out.code="OK";
    } catch(const Error& e) {
        out={e.code(),{},{}};
    } catch(...) {
        out={"INTERNAL_ERROR",{},{}};
    }
    const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()-metricStarted).count();
    constexpr std::array<long long,7> LatencyBounds{1000,5000,10000,25000,50000,100000,250000};
    std::lock_guard counting(outcomesMutex_);
    ++outcomes_[out.code];++metricOutcomes_[out.code];++metricOperations_["files.read"];
    for(std::size_t index=0;index<LatencyBounds.size();++index)
        if(elapsed<=LatencyBounds[index])++requestLatencyBuckets_[index];
    ++requestLatencyBuckets_.back();requestLatencyMicroseconds_+=static_cast<unsigned long long>(elapsed);
    return out;
}
std::map<std::string,unsigned long long> Service::takeOutcomes() {
    std::lock_guard counting(outcomesMutex_);return std::exchange(outcomes_,{});
}
Service::Metrics Service::metrics() {
    Metrics result;
    try {
        std::lock_guard lock(mutex_);
        Statement version(store_.db(),"PRAGMA user_version");(void)version.row();result.schemaVersion=version.number(0);
        Statement foreignKeys(store_.db(),"PRAGMA foreign_keys");(void)foreignKeys.row();
        Statement counts(store_.db(),"SELECT (SELECT COUNT(*) FROM titles),(SELECT COUNT(*) FROM users),"
            "(SELECT COUNT(*) FROM sessions WHERE expires>?1),(SELECT COUNT(*) FROM directory_sessions WHERE expires>?1),"
            "(SELECT COUNT(*) FROM session_invitations WHERE status='pending' AND expires>?1)");
        counts.bind(1,now());(void)counts.row();
        result.titles=counts.number(0);result.accounts=counts.number(1);
        result.activeAccessSessions=counts.number(2);result.activeDirectorySessions=counts.number(3);
        result.pendingInvitations=counts.number(4);
        Statement pages(store_.db(),"PRAGMA page_count");(void)pages.row();
        Statement pageSize(store_.db(),"PRAGMA page_size");(void)pageSize.row();
        result.databaseBytes=pages.number(0)*pageSize.number(0);
        result.ready=result.schemaVersion==SchemaVersion&&foreignKeys.number(0)==1;
    } catch(...) { result.ready=false; }
    std::lock_guard counting(outcomesMutex_);
    result.outcomes=metricOutcomes_;result.operations=metricOperations_;
    result.requestLatencyBuckets=requestLatencyBuckets_;
    result.requestLatencyMicroseconds=requestLatencyMicroseconds_;
    return result;
}
Json Service::identity(const std::string& id) {
    Statement s(store_.db(),"SELECT id,gamertag,motto,region,online_allowed,picture,gamer_zone FROM users WHERE id=?");s.bind(1,id);
    if (!s.row()) throw Error("NOT_FOUND");
    Statement score(store_.db(),"SELECT COALESCE(SUM(a.score),0),COUNT(*) FROM earned e JOIN achievements a ON a.game_id=e.game_id AND a.key=e.key WHERE e.user_id=?");
    score.bind(1,id);(void)score.row();
    // A title counts as played once the account has presence, an earned achievement or a leaderboard row in it.
    Statement titles(store_.db(),"SELECT COUNT(*) FROM (SELECT game_id FROM presence WHERE user_id=?1 UNION "
        "SELECT game_id FROM earned WHERE user_id=?1 UNION SELECT game_id FROM leaderboard_entries WHERE user_id=?1)");
    titles.bind(1,id);(void)titles.row();
    Json result{{"userId",s.text(0)},{"gamertag",s.text(1)},{"displayName",s.text(1)},{"motto",s.text(2)},
        {"region",s.text(3)},{"allowOnlineSessions",s.number(4)!=0},{"gamerScore",score.number(0)},{"totalAchievements",score.number(1)},
        {"titlesPlayed",titles.number(0)},{"picture",s.text(5)},{"gamerZone",s.text(6)}};
    // XNA GamerProfile.Reputation, 0 to 5 stars: the share of other players who would play with
    // this member again, in quarter stars. Only reviews make a reputation; without any, none is sent.
    Statement reviews(store_.db(),"SELECT COALESCE(SUM(rating='prefer'),0),COUNT(*) FROM player_reviews WHERE subject_id=?");
    reviews.bind(1,id);(void)reviews.row();
    if(reviews.number(1)>0)result["reputation"]=std::round(20.0*reviews.number(0)/reviews.number(1))/4.0;
    return result;
}
Json Service::dispatch(const Json& r,const std::string& peer,std::unique_lock<std::mutex>& lock) {
    const auto op=stringField(r,"op",64), game=stringField(r,"game",64), id=stringField(r,"id",64);
    const auto& a=r["args"];
    static const std::set<std::string> operations{"hello","auth.login","auth.logout","auth.refresh","auth.ping","gamer.lookup","profile.get","profile.gameDefaults","profile.setGameDefaults","profile.setGamerZone","friends.list","friends.add","friends.remove","friends.accept","presence.set","presence.status","achievements.list","achievements.award","assets.read","leaderboards.read","leaderboards.definition","leaderboards.list","leaderboards.game.begin","leaderboards.game.commit","leaderboards.game.abort","sessions.relayTicket","sessions.create","sessions.find","sessions.get","sessions.touch","sessions.update","sessions.join","sessions.joinInvited","sessions.leave","sessions.remove","sessions.addMembers","invites.send","invites.list","invites.get","invites.accept","invites.dismiss","invites.joinFriend","parties.get","parties.invite","parties.accept","parties.decline","parties.leave","messages.send","messages.list","messages.read","messages.delete","reviews.submit","avatars.get","avatars.set","avatars.catalog","avatars.catalogPack","privacy.block","privacy.unblock","privacy.list"};
    if (!operations.contains(op)) throw Error("UNKNOWN_OPERATION");
    if (op=="hello") return Json{{"version",1},{"capabilities",Json::array({"identity","authentication","session-refresh","heartbeat","friends","friend-requests","presence","presence-status","game-defaults","gamer-zone","achievements","assets","leaderboard-reads","leaderboard-list","title-version","local-leaderboard-commit","leaderboard-epoch-abort","ranked-arbitration","messages","player-reviews","avatars","avatar-catalog-packs","files","session-directory","session-removal","host-migration","session-add-members","session-invitations","join-friend","parties","events","relay-tickets","relay","request-outcomes","privacy","friend-voice","policy-refresh"})},{"maxMessageBytes",MaxMessageBytes}};
    Statement title(store_.db(),"SELECT id,minimum_version FROM titles WHERE id=?");title.bind(1,game);
    if (!title.row()) throw Error("UNKNOWN_TITLE");
    // A title may stop accepting old game versions (XNA GameUpdateRequiredException); a client that
    // states no version is as old as can be.
    if (const auto minimum=title.text(1);!minimum.empty()) {
        const auto version=r.contains("titleVersion")?stringField(r,"titleVersion",32):std::string{};
        if (version.empty()||compareVersions(version,minimum)<0) throw Error("UPDATE_REQUIRED");
    }
    std::string user,token;
    const auto timestamp=now();
    if (op!="auth.login"&&op!="auth.refresh") {
        token=stringField(r,"token",128);
        if (token.size()!=64) throw Error("UNAUTHENTICATED");
        Statement session(store_.db(),"SELECT user_id FROM sessions WHERE hash=? AND game_id=? AND expires>?");
        session.bind(1,sha256(token));session.bind(2,game);session.bind(3,timestamp);
        if (!session.row()) throw Error("UNAUTHENTICATED");
        user=session.text(0);
    }
    if (op=="auth.login"||op=="auth.refresh") {
        for (auto it=loginRates_.begin();it!=loginRates_.end();) {
            if (timestamp-it->second.start>=60) it=loginRates_.erase(it);else ++it;
        }
        if (loginRates_.size()>=4096 && !loginRates_.contains(peer)) throw Error("RATE_LIMITED");
        auto& rate=loginRates_[peer];if (rate.count++>=10) throw Error("RATE_LIMITED");if (!rate.start) rate.start=timestamp;
    }
    bool signedIn=false;
    if (op=="auth.login") {
        const auto username=stringField(a,"username",64),password=stringField(a,"password",256);
        if (identifier(username)&&password.size()>=8) {
            std::string salt="00000000000000000000000000000000",expected(64,'0'),account;bool found=false;
            {
                Statement s(store_.db(),"SELECT id,salt,verifier FROM users WHERE username=?");s.bind(1,username);
                if((found=s.row())) {account=s.text(0);salt=s.text(1);expected=s.text(2);}
            }
            // The scrypt derivation, tens of milliseconds by design, runs without the lock so other
            // players' requests keep flowing; no transaction is open on the shared connection here.
            lock.unlock();
            std::string computed;
            try {computed=passwordHash(password,salt);} catch(...) {lock.lock();throw;}
            lock.lock();
            signedIn=found&&expected.size()==computed.size()&&CRYPTO_memcmp(expected.data(),computed.data(),computed.size())==0;
            if(signedIn)user=account;
        }
    }
    const bool recorded=!Unrecorded.contains(op);
    // Whose request ID this is: the authenticated caller; nobody yet for a sign-in or refresh.
    const auto owner=token.empty()?std::string{}:user;
    // Settled before the transaction: SQLite refuses to change the sync level inside one.
    const DurableScope durable(store_,!Ephemeral.contains(op));
    // The request ID, the change it makes and its outcome commit together or not at all: a crash
    // before the commit leaves the ID unused, so the client's retry runs the request; a crash
    // after it leaves the outcome, so the retry is answered from the record.
    Store::Transaction transaction(store_);
    if (!token.empty()) {
        // Friends see a member online for 90 s after the last request. Fifteen seconds of slack
        // spares a write on nearly every request; the 30 s heartbeat always refreshes it.
        Statement seen(store_.db(),"UPDATE sessions SET last_seen=?1 WHERE hash=?2 AND last_seen<?1-15");
        seen.bind(1,timestamp);seen.bind(2,sha256(token));(void)seen.row();
    }
    if (recorded) {
        if (timestamp-lastTrim_>=60) {
            Statement trim(store_.db(),"DELETE FROM request_ids WHERE created<?");trim.bind(1,timestamp-86400);(void)trim.row();
            lastTrim_=timestamp;requestIdCounts_.clear();
            std::erase_if(requestBudgets_,[&](const auto& entry){return timestamp-entry.second.start>=86400;});
        }
        // Counted once per title and minute, then kept in memory, instead of on every request.
        auto counted=requestIdCounts_.find(game);
        if (counted==requestIdCounts_.end()) {
            Statement count(store_.db(),"SELECT COUNT(*) FROM request_ids WHERE game_id=?");count.bind(1,game);(void)count.row();
            counted=requestIdCounts_.emplace(game,count.number(0)).first;
        }
        fault("before-record");
        std::optional<std::string> outcome,stored;bool replayable=false;
        {
            Statement previous(store_.db(),"SELECT user_id,op,outcome,result FROM request_ids WHERE game_id=? AND id=?");
            previous.bind(1,game);previous.bind(2,id);
            if (previous.row()) {
                // A repeat is answered from the record when the same account repeats the same
                // operation and its outcome was kept; anything else is a reused ID.
                outcome=previous.text(2);
                if(!previous.null(3))stored=previous.text(3);
                replayable=previous.text(1)==op&&previous.text(0)==owner&&!outcome->empty()&&(*outcome!="OK"||stored);
            }
        }
        if (outcome) {
            if (!replayable) throw Error("DUPLICATE_REQUEST");
            transaction.commit();
            if (*outcome!="OK") throw Error(*outcome);
            return parse(*stored);
        }
        if (counted->second>=MaxTitleRequestIds) throw Error("LIMIT_EXCEEDED");
        if (!owner.empty()) {
            auto& budget=requestBudgets_[game+'\n'+owner];
            if (timestamp-budget.start>=86400) budget={timestamp,0};
            if (budget.count>=MaxAccountRequestIds) throw Error("RATE_LIMITED");
        }
        Statement nonce(store_.db(),"INSERT INTO request_ids(game_id,id,created) VALUES(?,?,?)");nonce.bind(1,game);nonce.bind(2,id);nonce.bind(3,timestamp);(void)nonce.row();
        fault("after-record");
    }
    Json result;std::string outcome="OK";
    try {
        if (op=="auth.login") {
            if (!signedIn) throw Error("AUTHENTICATION_FAILED");
            result=issueCredentials(user,game);
        } else result=execute(op,game,user,r,a,timestamp);
        // A response that cannot be sent is an outcome too; it is what a repeat will hear.
        if (result.dump().size()+256>MaxMessageBytes) throw Error("LIMIT_EXCEEDED");
    } catch (const Error& e) {
        // A refusal is the request's outcome and commits with whatever the request did before it
        // (a replayed refresh credential revokes its family, then refuses). An internal failure
        // rolls everything back, the ID included, so a retry runs it again.
        if (e.code()=="INTERNAL_ERROR") throw;
        outcome=e.code();
    }
    fault("before-commit");
    if (recorded) {
        // Results that carry a secret (access and refresh tokens, relay tickets) are never stored;
        // a repeat of those is refused as before.
        static const std::set<std::string> Secret{"auth.login","auth.refresh","sessions.relayTicket"};
        const auto stored=outcome=="OK"&&!Secret.contains(op)?result.dump():std::string{};
        Statement keep(store_.db(),"UPDATE request_ids SET user_id=?,op=?,outcome=?,result=? WHERE game_id=? AND id=?");
        keep.bind(1,owner);keep.bind(2,op);keep.bind(3,outcome);
        if(stored.empty()||stored.size()>MaxStoredResultBytes)keep.bindNull(4);else keep.bind(4,stored);
        keep.bind(5,game);keep.bind(6,id);(void)keep.row();
    }
    transaction.commit();
    fault("after-commit");
    if (recorded) {
        ++requestIdCounts_[game];
        if (!owner.empty()) ++requestBudgets_[game+'\n'+owner].count;
    }
    if (outcome!="OK") throw Error(outcome);
    return result;
}
Json Service::execute(const std::string& op,const std::string& game,const std::string& user,const Json& r,const Json& a,long long timestamp) {
    if(op=="auth.refresh")return refreshCredentials(game,a);
    if(op=="auth.ping") {
        // The client may say whether it can currently talk (XNA FriendGamer.HasVoice, capability
        // friend-voice); a ping without it leaves the last report.
        for(const auto& [key,value]:a.items()){(void)value;if(key!="voice")throw Error("INVALID_ARGUMENT");}
        if(a.contains("voice")) {
            if(!a["voice"].is_boolean())throw Error("INVALID_ARGUMENT");
            Statement voice(store_.db(),"UPDATE sessions SET voice=? WHERE hash=?");
            voice.bind(1,a["voice"].get<bool>()?1LL:0LL);voice.bind(2,sha256(stringField(r,"token",128)));(void)voice.row();
        }
        return Json{{"serverTime",timestamp},{"privileges",privileges(user)},{"blocked",privacy(user,"privacy.list",Json::object())["blocked"]}};
    }
    if (op=="auth.logout") {
        const auto hash=sha256(stringField(r,"token",128));Statement scope(store_.db(),"SELECT refresh_family FROM sessions WHERE hash=?");scope.bind(1,hash);(void)scope.row();
        const auto family=scope.text(0);if(!family.empty())revokeFamily(family);
        else {Statement remove(store_.db(),"DELETE FROM sessions WHERE hash=?");remove.bind(1,hash);(void)remove.row();}
        return Json::object();
    }
    if (op=="gamer.lookup" || op=="profile.get") {
        const auto gamertag=stringField(a,"gamertag",32);
        Statement s(store_.db(),"SELECT id FROM users WHERE gamertag=?");s.bind(1,gamertag);
        if (!s.row()) throw Error("NOT_FOUND");
        mayView(user,s.text(0));
        return identity(s.text(0));
    }
    if (op=="presence.set") {
        if (!a.contains("mode") || !a["mode"].is_number_integer() || a["mode"]<0 || a["mode"]>255) throw Error("INVALID_ARGUMENT");
        Statement s(store_.db(),"INSERT INTO presence(user_id,game_id,mode,text) VALUES(?,?,?,?) ON CONFLICT(user_id,game_id) DO UPDATE SET mode=excluded.mode,text=excluded.text");
        s.bind(1,user);s.bind(2,game);s.bind(3,a["mode"].get<long long>());s.bind(4,stringField(a,"text",256));(void)s.row();return Json::object();
    }
    if (op=="profile.gameDefaults"||op=="profile.setGameDefaults") {
        // XNA SignedInGamer.GameDefaults: the caller's own preferred game settings, account-wide.
        if(op=="profile.setGameDefaults") {
            if(a.size()!=1||!a.contains("gameDefaults"))throw Error("INVALID_ARGUMENT");
            validateGameDefaults(a["gameDefaults"]);
            Statement update(store_.db(),"UPDATE users SET game_defaults=? WHERE id=?");update.bind(1,a["gameDefaults"].dump());update.bind(2,user);(void)update.row();
        } else if(!a.empty())throw Error("INVALID_ARGUMENT");
        Statement read(store_.db(),"SELECT game_defaults FROM users WHERE id=?");read.bind(1,user);(void)read.row();
        return Json{{"gameDefaults",parse(read.text(0))}};
    }
    if (op=="profile.setGamerZone") {
        // The member's own choice; "unknown" clears it.
        const auto zone=stringField(a,"gamerZone",16);
        if(zone!="unknown"&&zone!="recreation"&&zone!="pro"&&zone!="family"&&zone!="underground")throw Error("INVALID_ARGUMENT");
        Statement update(store_.db(),"UPDATE users SET gamer_zone=? WHERE id=?");update.bind(1,zone);update.bind(2,user);(void)update.row();
        return identity(user);
    }
    if (op=="presence.status") {
        // Account-wide, like the console's online status; friends see it only while online.
        const auto status=stringField(a,"status",8);
        if(status!="online"&&status!="away"&&status!="busy") throw Error("INVALID_ARGUMENT");
        Statement s(store_.db(),"UPDATE users SET status=? WHERE id=?");s.bind(1,status);s.bind(2,user);(void)s.row();
        return Json{{"status",status}};
    }
    if (op=="friends.add" || op=="friends.remove" || op=="friends.accept") {
        Statement target(store_.db(),"SELECT id FROM users WHERE gamertag=?");target.bind(1,stringField(a,"gamertag",32));
        if (!target.row()) throw Error("NOT_FOUND");
        if (target.text(0)==user) throw Error("INVALID_ARGUMENT");
        const auto friendId=target.text(0);
        if(op!="friends.remove")mayCommunicate(user,friendId,true);
        if(op=="friends.accept") {
            Statement incoming(store_.db(),"SELECT 1 FROM friends WHERE user_id=? AND friend_id=?");
            incoming.bind(1,friendId);incoming.bind(2,user);if(!incoming.row())throw Error("INVALID_STATE");
        }
        if(op!="friends.remove") {
            Statement existing(store_.db(),"SELECT 1 FROM friends WHERE user_id=? AND friend_id=?");
            existing.bind(1,user);existing.bind(2,friendId);
            if(!existing.row())for(const auto& account:{user,friendId}) {
                Statement cap(store_.db(),"SELECT COUNT(*) FROM friends WHERE user_id=? OR friend_id=?");
                cap.bind(1,account);cap.bind(2,account);(void)cap.row();if(cap.number(0)>=256)throw Error("LIMIT_EXCEEDED");
            }
        }
        Statement s(store_.db(),op=="friends.remove"?
            "DELETE FROM friends WHERE (user_id=? AND friend_id=?) OR (user_id=? AND friend_id=?)":
            "INSERT OR IGNORE INTO friends(user_id,friend_id) VALUES(?,?)");
        s.bind(1,user);s.bind(2,friendId);
        if(op=="friends.remove"){s.bind(3,friendId);s.bind(4,user);}
        (void)s.row();hint(friendId,"friends");return Json::object();
    }
    if (op=="friends.list") {
        Statement s(store_.db(),"SELECT u.id,u.gamertag,EXISTS(SELECT 1 FROM sessions z WHERE z.user_id=u.id AND z.expires>? AND z.last_seen>?),COALESCE(p.mode,0),COALESCE(p.text,''),EXISTS(SELECT 1 FROM friends f WHERE f.user_id=? AND f.friend_id=u.id),EXISTS(SELECT 1 FROM friends f WHERE f.friend_id=? AND f.user_id=u.id),u.status,"
            "EXISTS(SELECT 1 FROM sessions z WHERE z.user_id=u.id AND z.expires>?1 AND z.last_seen>?2 AND z.voice=1) AND u.privilege_communication!='blocked' FROM users u LEFT JOIN presence p ON p.user_id=u.id AND p.game_id=? WHERE u.id IN (SELECT friend_id FROM friends WHERE user_id=? UNION SELECT user_id FROM friends WHERE friend_id=?) ORDER BY u.gamertag LIMIT 257");
        s.bind(1,timestamp);s.bind(2,timestamp-90);s.bind(3,user);s.bind(4,user);s.bind(5,game);s.bind(6,user);s.bind(7,user);
        Json friends=Json::array();
        while(s.row()) {
            if(friends.size()>=256)throw Error("LIMIT_EXCEEDED");
            const bool accepted=s.number(5)&&s.number(6), online=accepted&&s.number(2);
            Json row{{"userId",s.text(0)},{"gamertag",s.text(1)},{"online",online},
                {"presenceMode",online?s.number(3):0},{"presenceText",online?s.text(4):""},
                {"away",online&&s.text(7)=="away"},{"busy",online&&s.text(7)=="busy"},{"hasVoice",online&&s.number(8)!=0},
                {"accepted",accepted},{"requestSent",!accepted&&s.number(5)!=0},{"requestReceived",!accepted&&s.number(6)!=0},
                {"joinable",false},{"inviteReceivedFrom",false},{"inviteSentTo",false},{"inviteAccepted",false},{"inviteRejected",false}};
            if(accepted) {
                // Joinable: in this title's player-match session that is live, admits joiners now and has a public slot.
                Statement joinable(store_.db(),"SELECT EXISTS(SELECT 1 FROM directory_members m JOIN directory_sessions d ON d.id=m.session_id "
                    "WHERE m.game_id=? AND m.user_id=? AND d.kind='player' AND d.expires>? AND (d.state='lobby' OR d.allow_join=1) "
                    "AND (SELECT COUNT(*) FROM directory_members o WHERE o.session_id=d.id AND o.private_slot=0)<d.max_gamers-d.private_slots)");
                joinable.bind(1,game);joinable.bind(2,s.text(0));joinable.bind(3,timestamp);(void)joinable.row();
                // This title's unexpired invitations between the two, as XNA's friend state reports them.
                Statement invites(store_.db(),"SELECT "
                    "EXISTS(SELECT 1 FROM session_invitations WHERE game_id=?1 AND sender_id=?2 AND recipient_id=?3 AND status='pending' AND expires>?4 AND requested=0),"
                    "EXISTS(SELECT 1 FROM session_invitations WHERE game_id=?1 AND sender_id=?3 AND recipient_id=?2 AND status='pending' AND expires>?4 AND requested=0),"
                    "EXISTS(SELECT 1 FROM session_invitations WHERE game_id=?1 AND sender_id=?3 AND recipient_id=?2 AND status IN ('accepted','used') AND expires>?4 AND requested=0),"
                    "EXISTS(SELECT 1 FROM session_invitations WHERE game_id=?1 AND sender_id=?3 AND recipient_id=?2 AND status='dismissed' AND expires>?4 AND requested=0)");
                invites.bind(1,game);invites.bind(2,s.text(0));invites.bind(3,user);invites.bind(4,timestamp);(void)invites.row();
                row["joinable"]=online&&joinable.number(0)!=0;
                row["inviteReceivedFrom"]=invites.number(0)!=0;row["inviteSentTo"]=invites.number(1)!=0;
                row["inviteAccepted"]=invites.number(2)!=0;row["inviteRejected"]=invites.number(3)!=0;
            }
            friends.push_back(std::move(row));
        }
        return Json{{"friends",friends}};
    }
    if(op=="assets.read") {
        const auto hash=stringField(a,"hash",64);
        if(hash.size()!=64||hash.find_first_not_of("0123456789abcdef")!=std::string::npos)throw Error("INVALID_ARGUMENT");
        for(const auto* key:{"offset","length"})if(!a.contains(key)||!a[key].is_number_integer()||a[key]<0)throw Error("INVALID_ARGUMENT");
        const auto offset=a["offset"].get<long long>(),length=a["length"].get<long long>();
        if(offset>16777216||length<1||length>12288)throw Error("LIMIT_EXCEEDED");
        if(!mayReadAsset(user,game,hash))throw Error("NOT_FOUND");
        Statement asset(store_.db(),"SELECT size,mime,substr(bytes,?,?) FROM assets WHERE hash=?");
        asset.bind(1,offset+1);asset.bind(2,length);asset.bind(3,hash);if(!asset.row())throw Error("NOT_FOUND");
        if(offset>=asset.number(0))throw Error("INVALID_ARGUMENT");
        const auto bytes=asset.blob(2);constexpr char digits[]="0123456789abcdef";std::string encoded;encoded.reserve(bytes.size()*2);
        for(unsigned char byte:bytes){encoded+=digits[byte>>4];encoded+=digits[byte&15];}
        return Json{{"hash",hash},{"size",asset.number(0)},{"mime",asset.text(1)},{"offset",offset},{"hex",encoded}};
    }
    if(op=="messages.send") {
        // Guide Compose: 1..100 recipients, text <=256 UTF-8 bytes; bounded inbox and sender rate.
        const auto text=stringField(a,"text",256);
        if(!a.contains("gamertags")||!a["gamertags"].is_array()||a["gamertags"].empty()||a["gamertags"].size()>100)throw Error("INVALID_ARGUMENT");
        Statement rate(store_.db(),"SELECT COUNT(*) FROM messages WHERE sender_id=? AND created>?");rate.bind(1,user);rate.bind(2,timestamp-3600);(void)rate.row();
        if(rate.number(0)+static_cast<long long>(a["gamertags"].size())>200)throw Error("RATE_LIMITED");
        std::vector<std::string> recipients;std::set<std::string> seen;
        for(const auto& tag:a["gamertags"]) {
            if(!tag.is_string())throw Error("INVALID_ARGUMENT");
            Statement target(store_.db(),"SELECT id FROM users WHERE gamertag=?");target.bind(1,tag.get<std::string>());
            if(!target.row())throw Error("NOT_FOUND");
            if(target.text(0)==user||!seen.insert(target.text(0)).second)throw Error("INVALID_ARGUMENT");
            mayCommunicate(user,target.text(0),false);
            Statement inbox(store_.db(),"SELECT COUNT(*) FROM messages WHERE recipient_id=?");inbox.bind(1,target.text(0));(void)inbox.row();
            if(inbox.number(0)>=100)throw Error("LIMIT_EXCEEDED");
            recipients.push_back(target.text(0));
        }
        Store::Transaction transaction(store_);
        for(const auto& recipient:recipients) {
            Statement insert(store_.db(),"INSERT INTO messages(id,sender_id,recipient_id,text,created) VALUES(?,?,?,?,?)");
            insert.bind(1,randomHex(16));insert.bind(2,user);insert.bind(3,recipient);insert.bind(4,text);insert.bind(5,timestamp);(void)insert.row();
            hint(recipient,"messages");
        }
        transaction.commit();
        return Json{{"sent",recipients.size()}};
    }
    if(op=="messages.list") {
        const auto start=integerField(a,"start",0,100),limit=integerField(a,"limit",1,32);
        Statement total(store_.db(),"SELECT COUNT(*),SUM(read=0) FROM messages WHERE recipient_id=?");total.bind(1,user);(void)total.row();
        Statement page(store_.db(),"SELECT m.id,u.gamertag,m.text,m.created,m.read FROM messages m JOIN users u ON u.id=m.sender_id WHERE m.recipient_id=? ORDER BY m.created DESC,m.id LIMIT ? OFFSET ?");
        page.bind(1,user);page.bind(2,limit);page.bind(3,start);
        Json rows=Json::array();
        while(page.row())rows.push_back(Json{{"message",page.text(0)},{"sender",page.text(1)},{"text",page.text(2)},{"created",page.number(3)},{"read",page.number(4)!=0}});
        return Json{{"start",start},{"total",total.number(0)},{"unread",total.number(1)},{"messages",rows}};
    }
    if(op=="messages.read"||op=="messages.delete") {
        const auto id=stringField(a,"message",32);
        if(id.size()!=32||id.find_first_not_of("0123456789abcdef")!=std::string::npos)throw Error("INVALID_ARGUMENT");
        Statement owned(store_.db(),"SELECT 1 FROM messages WHERE id=? AND recipient_id=?");owned.bind(1,id);owned.bind(2,user);
        if(!owned.row())throw Error("NOT_FOUND");
        Statement change(store_.db(),op=="messages.read"?"UPDATE messages SET read=1 WHERE id=?":"DELETE FROM messages WHERE id=?");change.bind(1,id);(void)change.row();
        return Json::object();
    }
    if(op=="reviews.submit") {
        // Guide Player Review: prefer/avoid feedback; "clear" withdraws it. Avoided hosts are not
        // offered by this reviewer's matchmaking searches.
        const auto rating=stringField(a,"rating",16);
        if(rating!="prefer"&&rating!="avoid"&&rating!="clear")throw Error("INVALID_ARGUMENT");
        Statement target(store_.db(),"SELECT id FROM users WHERE gamertag=?");target.bind(1,stringField(a,"gamertag",32));
        if(!target.row())throw Error("NOT_FOUND");
        if(target.text(0)==user)throw Error("INVALID_ARGUMENT");
        Statement cap(store_.db(),"SELECT COUNT(*) FROM player_reviews WHERE reviewer_id=?");cap.bind(1,user);(void)cap.row();
        if(rating!="clear"&&cap.number(0)>=1024)throw Error("LIMIT_EXCEEDED");
        Statement change(store_.db(),rating=="clear"?"DELETE FROM player_reviews WHERE reviewer_id=? AND subject_id=?":
            "INSERT INTO player_reviews(reviewer_id,subject_id,rating,updated) VALUES(?,?,?,?) ON CONFLICT(reviewer_id,subject_id) DO UPDATE SET rating=excluded.rating,updated=excluded.updated");
        change.bind(1,user);change.bind(2,target.text(0));if(rating!="clear"){change.bind(3,rating);change.bind(4,timestamp);}(void)change.row();
        return Json::object();
    }
    if(op.starts_with("avatars."))return avatars(user,op,a,timestamp);
    if(op.starts_with("privacy."))return privacy(user,op,a);
    if(op.starts_with("invites."))return invitations(user,game,op,a);
    if(op.starts_with("parties."))return parties(user,game,op,a);
    if(op=="sessions.relayTicket")return issueRelayTicket(user,game,a);
    if(op.starts_with("sessions."))return directory(user,game,op,a);
    if(op=="leaderboards.game.begin")return beginLeaderboardGame(user,game,a);
    if(op=="leaderboards.game.abort")return abortLeaderboardGame(user,game,a);
    if(op=="leaderboards.game.commit")return commitLeaderboardGame(user,game,a);
    if(op=="leaderboards.read")return readLeaderboard(game,a);
    if(op=="leaderboards.list") {
        // The title's provisioned boards, for a system leaderboard page; a game reads its own by identity.
        Statement boards(store_.db(),"SELECT b.key,b.mode,b.ascending,b.arbitrated,(SELECT COUNT(*) FROM leaderboard_entries e WHERE e.game_id=b.game_id AND e.key=b.key AND e.mode=b.mode) FROM leaderboards b WHERE b.game_id=? ORDER BY b.key,b.mode LIMIT 257");
        boards.bind(1,game);
        Json list=Json::array();
        while(boards.row()) {
            if(list.size()>=256)throw Error("LIMIT_EXCEEDED");
            list.push_back(Json{{"key",boards.text(0)},{"mode",boards.number(1)},{"ascending",boards.number(2)!=0},{"arbitrated",boards.number(3)!=0},{"entries",boards.number(4)}});
        }
        return Json{{"boards",list}};
    }
    if(op=="leaderboards.definition") {
        Statement board(store_.db(),"SELECT ascending,aggregation,arbitrated,columns FROM leaderboards WHERE game_id=? AND key=? AND mode=?");
        board.bind(1,game);board.bind(2,stringField(a,"key",64));board.bind(3,integerField(a,"mode",-2147483648LL,2147483647LL));
        if(!board.row())throw Error("NOT_FOUND");
        return Json{{"ascending",board.number(0)!=0},{"aggregation",board.text(1)},{"arbitrated",board.number(2)!=0},{"columns",parse(board.text(3))}};
    }
    if (op=="achievements.award") {
        const auto key=stringField(a,"key",64);if(!identifier(key))throw Error("INVALID_ARGUMENT");
        Statement definition(store_.db(),"SELECT key,name FROM achievements WHERE game_id=? AND key=?");definition.bind(1,game);definition.bind(2,key);
        if(!definition.row())throw Error("NOT_FOUND");
        Statement s(store_.db(),"INSERT OR IGNORE INTO earned(user_id,game_id,key,ticks) VALUES(?,?,?,?)");
        s.bind(1,user);s.bind(2,game);s.bind(3,key);s.bind(4,(timestamp+62135596800LL)*10000000LL);(void)s.row();
        // Whether this call earned it (a client shows "achievement unlocked" once), and its name.
        return Json{{"awarded",sqlite3_changes(store_.db())>0},{"name",definition.text(1)}};
    }
    Statement s(store_.db(),"SELECT a.key,a.name,a.description,a.how_to_earn,a.score,a.display,a.picture,COALESCE(e.ticks,0) FROM achievements a LEFT JOIN earned e ON e.game_id=a.game_id AND e.key=a.key AND e.user_id=? WHERE a.game_id=? ORDER BY a.key LIMIT 129");
    s.bind(1,user);s.bind(2,game);Json achievements=Json::array();
    while(s.row()) { if(achievements.size()>=128)throw Error("LIMIT_EXCEEDED");achievements.push_back(Json{{"key",s.text(0)},{"name",s.text(1)},{"description",s.text(2)},{"howToEarn",s.text(3)},{"score",s.number(4)},{"display",s.number(5)!=0},{"picture",s.text(6)},{"earnedTicks",s.number(7)}}); }
    return Json{{"achievements",achievements}};
}
}
