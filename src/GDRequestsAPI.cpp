#include "GDRequestsAPI.h"
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>

static const std::string SERVER = "https://www.gdrequests.org";

void GDRequestsAPI::sendQueueAction(const std::string& endpoint, const std::string& levelId) {
    auto token = Mod::get()->getSettingValue<std::string>("creator-token");
    if (token.empty()) return;

    std::string url = SERVER + endpoint;
    auto bodyObj = matjson::makeObject({
        {"token", token},
        {"level_id", levelId}
    });
    std::string body = bodyObj.dump();

    geode::async::spawn(
        [url, body]() -> web::WebFuture {
            return web::WebRequest()
                .header("Content-Type", "application/json")
                .body(std::vector<uint8_t>(body.begin(), body.end()))
                .post(url);
        },
        [](web::WebResponse) {}
    );
}

void GDRequestsAPI::sendQueueRemoveYoutube(const std::string& youtubeUrl) {
    auto token = Mod::get()->getSettingValue<std::string>("creator-token");
    if (token.empty()) return;

    std::string url = SERVER + "/api/queue/remove";
    auto bodyObj = matjson::makeObject({
        {"token", token},
        {"youtube_url", youtubeUrl}
    });
    std::string body = bodyObj.dump();

    geode::async::spawn(
        [url, body]() -> web::WebFuture {
            return web::WebRequest()
                .header("Content-Type", "application/json")
                .body(std::vector<uint8_t>(body.begin(), body.end()))
                .post(url);
        },
        [](web::WebResponse) {}
    );
}

void GDRequestsAPI::sendTimeoutUser(const std::string& username) {
    auto token = Mod::get()->getSettingValue<std::string>("creator-token");
    if (token.empty()) return;

    std::string url = SERVER + "/api/queue/timeout";
    auto bodyObj = matjson::makeObject({
        {"token", token},
        {"username", username}
    });
    std::string body = bodyObj.dump();

    geode::async::spawn(
        [url, body]() -> web::WebFuture {
            return web::WebRequest()
                .header("Content-Type", "application/json")
                .body(std::vector<uint8_t>(body.begin(), body.end()))
                .post(url);
        },
        [username](web::WebResponse res) {
            if (res.ok()) {
                auto j = res.json();
                int mins = 60;
                if (j && (*j).contains("duration_mins"))
                    mins = (*j)["duration_mins"].asInt().unwrapOr(60);
                Notification::create(
                    fmt::format("{} timed out for {} min", username, mins),
                    NotificationIcon::Warning, 3.f
                )->show();
            }
        }
    );
}

void GDRequestsAPI::sendQueueRemoveAll() {
    auto token = Mod::get()->getSettingValue<std::string>("creator-token");
    if (token.empty()) return;

    std::string url = SERVER + "/api/queue/remove-all";
    auto bodyObj = matjson::makeObject({
        {"token", token}
    });
    std::string body = bodyObj.dump();

    geode::async::spawn(
        [url, body]() -> web::WebFuture {
            return web::WebRequest()
                .header("Content-Type", "application/json")
                .body(std::vector<uint8_t>(body.begin(), body.end()))
                .post(url);
        },
        [](web::WebResponse) {}
    );
}

void GDRequestsAPI::fetchQueue(std::function<void(std::vector<QueueEntry>)> callback) {
    auto gv = GlobalVars::getSharedInstance();
    if (gv->fetchInProgress) return;
    gv->fetchInProgress = true;

    auto token = Mod::get()->getSettingValue<std::string>("creator-token");
    if (token.empty()) {
        gv->fetchInProgress = false;
        FLAlertLayer::create(
            "GD Requests",
            "No token set! Go to Mods > GD Requests > Settings and paste your creator token from gdrequests.org.",
            "OK"
        )->show();
        return;
    }

    std::string queueUrl = SERVER + "/api/queue/" + token;

    geode::async::spawn(
        [queueUrl]() -> web::WebFuture {
            return web::WebRequest().get(queueUrl);
        },
        [token, callback](web::WebResponse res) {
            auto gv = GlobalVars::getSharedInstance();
            if (!res.ok()) {
                gv->fetchInProgress = false;
                std::string msg = res.code() == 404
                    ? "Creator token not recognised. Double-check the token in Mods > GD Requests > Settings."
                    : "Could not reach the server. Check your internet connection.";
                FLAlertLayer::create("GD Requests", msg.c_str(), "OK")->show();
                return;
            }

            auto jsonRes = res.json();
            if (!jsonRes) {
                gv->fetchInProgress = false;
                FLAlertLayer::create("GD Requests", "Invalid server response.", "OK")->show();
                return;
            }

            std::vector<QueueEntry> entries;
            gv->queueLevelIds.clear();
            gv->queueLevelNames.clear();
            gv->currentLevelList.clear();

            auto& json = *jsonRes;
            if (json.contains("requests") && json["requests"].isArray()) {
                for (auto& item : json["requests"]) {
                    QueueEntry qe;
                    qe.name         = item["name"].asString().unwrapOr("Unknown");
                    qe.levelId      = item["level_id"].asString().unwrapOr("");
                    qe.youtubeUrl   = item["youtube_url"].asString().unwrapOr("");
                    qe.levelName    = item["level_name"].asString().unwrapOr("");
                    qe.gdDifficulty = item["gd_difficulty"].asString().unwrapOr("");
                    qe.source       = item["source"].asString().unwrapOr(""); 
                    if (!qe.levelId.empty() || !qe.youtubeUrl.empty()) {
                        if (!qe.levelId.empty()) {
                            gv->queueLevelIds.insert(qe.levelId);
                            gv->queueLevelNames[qe.levelId] = qe.name;
                        }
                        entries.push_back(qe);
                        gv->currentLevelList.push_back(std::move(qe));
                    }
                }
            }

            // Check chatter status
            if (entries.empty()) {
                gv->fetchInProgress = false;
                callback({});
                return;
            }

            std::string names;
            for (auto& e : entries) {
                if (!names.empty()) names += ",";
                names += e.name;
            }
            std::string statusUrl = SERVER + "/api/chatter-status?token=" + token + "&names=" + names;

            geode::async::spawn(
                [statusUrl]() -> web::WebFuture {
                    return web::WebRequest().get(statusUrl);
                },
                [entries = std::move(entries), callback](web::WebResponse statusRes) mutable {
                    auto gv = GlobalVars::getSharedInstance();
                    gv->fetchInProgress = false;
                    if (statusRes.ok()) {
                        auto sJson = statusRes.json();
                        if (sJson) {
                            for (auto& e : entries) {
                                if ((*sJson).contains(e.name))
                                    e.online = (*sJson)[e.name].asBool().unwrapOr(false);
                            }
                        }
                    }
                    callback(std::move(entries));
                }
            );
        }
    );
}

GJGameLevel* GDRequestsAPI::parseQueueEntryToLevel(const QueueEntry& entry) {
    GJGameLevel* level = GJGameLevel::create();
    level->m_levelID = entry.levelId.empty() ? 0 : std::stoi(entry.levelId);
    level->m_levelName = entry.levelName.empty() ? (entry.levelId.empty() ? "YouTube Request" : "ID: " + entry.levelId) : entry.levelName;
    level->m_creatorName = "-"; // We don't have creator name in QueueEntry usually
    
    // Custom fields logic from original mod
    // Note: You might need to adjust based on how you handle fields in Geode
    // For now, let's just populate standard fields
    
    return level;
}

GJGameLevel* GDRequestsAPI::parseJsonToLevel(const matjson::Value& levelJson) {
    // This is the original logic from ServerListener.cpp
    std::string creator = levelJson["creator"].asString().unwrapOr("-");
    std::string name = levelJson["name"].asString().unwrapOr("Unknown");
    int ID = levelJson["id"].asInt().unwrapOr(0);
    
    GJGameLevel* levelData = GJGameLevel::create();
    levelData->m_levelID = ID;
    levelData->m_levelName = name;
    levelData->m_creatorName = creator;
    
    // ... more field parsing if needed ...
    return levelData;
}
