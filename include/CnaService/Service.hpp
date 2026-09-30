// SPDX-License-Identifier: MIT
#pragma once
#include "CnaService/Store.hpp"
#include "CnaService/Avatars.hpp"
#include <mutex>
#include "CnaService/RelayAuthorization.hpp"
#include <functional>
#include <map>
#include <utility>
#include <vector>
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
    /** @brief One file for the binary route. */
    struct File {
        /** @brief OK or the error code (UNAUTHENTICATED, NOT_FOUND, RATE_LIMITED, INVALID_ARGUMENT, ...). */
        std::string code;
        /** @brief Media type. */
        std::string mime;
        /** @brief Contents. */
        std::string bytes;
    };
    /** @brief Serves one immutable file by content (GET /cna/v1/files/<sha256>): a title asset, an
     * account picture, a file of an imported avatar catalog or a catalog manifest, to a signed-in
     * account of the title, within its download budget. @param game Title. @param token Access
     * token; never logged. @param hash Lower-case hex SHA-256. @return File or refusal. */
    File file(std::string_view game,std::string_view token,std::string_view hash);
    /** @brief Takes the count of responses per error code since the previous call, for the
     * operator's periodic statistics. @return Code to count; OK included. */
    std::map<std::string,unsigned long long> takeOutcomes();
    /** @brief Redeems short-lived one-use authority on an encrypted relay connection.
     * @param game Title. @param ticket Secret from HTTPS; never logged. @return Server-owned grant. */
    RelayGrant redeemRelayTicket(const std::string& game,const std::string& ticket);
    /** @brief Revalidates membership, title, every local account and revocable authority.
     * @param grant Server-owned grant. @return Whether connection remains authorized. */
    bool validateRelayGrant(const RelayGrant& grant);
    /** @brief Releases a disconnected grant without touching account/directory state.
     * @param grant Exact server-owned grant. */
    void releaseRelayGrant(const RelayGrant& grant);
    /** @brief Where push hints go: after a request succeeds, each other account it changed and what
     * changed for it ("invitations", "messages", "friends" or "party"). Called outside the service
     * lock, on the request's worker thread. @param sink Receiver. */
    void setHintSink(std::function<void(const std::string& user,const std::string& topic)> sink);
    /** @brief Called at named points of a request ("before-record", "after-record", "hint",
     * "before-commit", "after-commit"). */
    using FaultHook = void (*)(const char* point);
    /** @brief For crash tests only: runs a hook, for instance one that ends the process, at each
     * named point of every request. Never set by the server. @param hook Hook or nullptr. */
    static void setFaultHookForTesting(FaultHook hook);
    /** @brief Authenticates an event channel. @param game Title. @param token Access token; never
     * logged. @return The account the live token belongs to; throws UNAUTHENTICATED otherwise. */
    std::string eventAccount(std::string_view game,std::string_view token);
private:
    void hint(const std::string& user,const char* topic);
    std::vector<std::pair<std::string,std::string>> hints_;
    std::function<void(const std::string&,const std::string&)> hintSink_;
    Json issueRelayTicket(const std::string& user,const std::string& game,const Json& args);
    bool validateRelayGrantLocked(const RelayGrant& grant,bool redeemed);
    void pruneRelayTickets();
    Json dispatch(const Json& request,const std::string& peer,std::unique_lock<std::mutex>& lock);
    Json execute(const std::string& op,const std::string& game,const std::string& user,const Json& request,const Json& args,long long timestamp);
    static void fault(const char* point);
    static FaultHook faultHook_;
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
    Json parties(const std::string& user,const std::string& game,const std::string& op,const Json& args);
    Json avatars(const std::string& user,const std::string& op,const Json& args,long long now);
    const CatalogInfo& catalog(long long version);
    Json invitationSnapshot(const std::string& id);
    bool friendsWith(const std::string& a,const std::string& b);
    void mayCommunicate(const std::string& from,const std::string& to,bool friendRequest);
    void mayView(const std::string& viewer,const std::string& target);
    Json privileges(const std::string& user);
    Json privacy(const std::string& user,const std::string& op,const Json& args);

    Store store_;
    std::mutex mutex_;
    struct Rate { long long start=0; int count=0; };
    std::map<std::string,Rate> loginRates_;
    long long lastTrim_=0;
    std::mutex outcomesMutex_;
    std::map<std::string,unsigned long long> outcomes_;
    std::map<std::string,long long> requestIdCounts_;
    std::map<std::string,Rate> requestBudgets_;
    std::map<long long,CatalogInfo> catalogs_;
    struct Budget { long long start=0; long long bytes=0; };
    std::map<std::string,Budget> downloads_;
};
}
