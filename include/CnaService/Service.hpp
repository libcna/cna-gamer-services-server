// SPDX-License-Identifier: MIT
#pragma once
#include "CnaService/Store.hpp"
#include <mutex>
#include "CnaService/RelayAuthorization.hpp"
#include <map>
namespace CnaService {
/** @brief Title-isolated logical service; transport-independent. */
class Service {
public:
    /** @brief Opens service storage. @param database SQLite path. */
    explicit Service(const std::string& database);
    /** @brief Handles an untrusted request without exposing internal diagnostics.
     * @param bytes Request bytes. @param peer Server-derived source identity.
     * @return Response bytes. */
    std::string handle(std::string_view bytes,std::string_view peer);
    /** @brief Redeems short-lived one-use authority on an encrypted relay connection.
     * @param game Title. @param ticket Secret from HTTPS; never logged. @return Server-owned grant. */
    RelayGrant redeemRelayTicket(const std::string& game,const std::string& ticket);
    /** @brief Revalidates membership, title, every local account and revocable authority.
     * @param grant Server-owned grant. @return Whether connection remains authorized. */
    bool validateRelayGrant(const RelayGrant& grant);
    /** @brief Releases a disconnected grant without touching account/directory state.
     * @param grant Exact server-owned grant. */
    void releaseRelayGrant(const RelayGrant& grant);
private:
    Json issueRelayTicket(const std::string& user,const std::string& game,const Json& args);
    bool validateRelayGrantLocked(const RelayGrant& grant,bool redeemed);
    void pruneRelayTickets();
    Json dispatch(const Json& request,const std::string& peer,std::unique_lock<std::mutex>& lock);
    Json identity(const std::string& id);
    Json readLeaderboard(const std::string& game,const Json& args);
    Json beginLeaderboardGame(const std::string& user,const std::string& game,const Json& args);
    Json commitLeaderboardGame(const std::string& user,const std::string& game,const Json& args);
    Json abortLeaderboardGame(const std::string& user,const std::string& game,const Json& args);
    void resolveArbitration(const std::string& game,const std::string& round);
    Json issueCredentials(const std::string& user,const std::string& game,const std::string& family={});
    Json refreshCredentials(const std::string& game,const Json& args);
    void revokeFamily(const std::string& family);
    Json directory(const std::string& user,const std::string& game,const std::string& op,const Json& args);
    Json directorySnapshot(const std::string& id,bool includeMembers);
    std::vector<std::string> directoryParticipants(const std::string& user,const std::string& game,const Json& args,bool includesActor=true);
    void pruneDirectory();
    bool migrateDirectoryHost(const std::string& session,const std::string& departing);
    Json invitations(const std::string& user,const std::string& game,const std::string& op,const Json& args);
    Json avatars(const std::string& user,const std::string& op,const Json& args,long long now);
    Json invitationSnapshot(const std::string& id);

    Store store_;
    std::mutex mutex_;
    struct Rate { long long start=0; int count=0; };
    std::map<std::string,Rate> loginRates_;
    long long lastTrim_=0;
    std::map<std::string,long long> requestIdCounts_;
    std::map<std::string,Rate> requestBudgets_;
};
}
