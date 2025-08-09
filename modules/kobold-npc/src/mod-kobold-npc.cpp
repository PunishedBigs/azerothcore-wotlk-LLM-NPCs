#include "Player.h"
#include "ScriptMgr.h"
#include "Chat.h"
#include "Creature.h"
#include "ObjectGuid.h"
#include "MapMgr.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include <thread>
#include <mutex>
#include <queue>
#include <fstream>
#include <sstream> // Required for std::ostringstream
#include <iomanip> // Required for std::fixed and std::setprecision
#include "json.hpp"
#include "httplib.h"

//==============================================================================
// Global AI Configuration & Structures
//==============================================================================
struct AiConfig
{
    // Network
    std::string address = "127.0.0.1:5001";
    std::string host = "127.0.0.1";
    int port = 5001;

    // Samplers (Restored to original defaults)
    int max_context_length = 8192;
    int max_length = 128;
    float temperature = 0.8f;
    float repetition_penalty = 1.1f;
    float top_p = 0.9f;
    int top_k = 40;

    // Other
    std::string stop_sequence = "\\n||$||Player:||$||[INST]||$||</s>";
    std::string system_prompt = "You are a helpful AI assistant roleplaying as a character in the World of Warcraft.\nFollow these rules strictly:\n1. Always stay in character.\n2. Do not use newline characters in your response.\n3. Keep your responses to a single, concise paragraph.\n4. Never speak for the player.";
    std::map<std::string, std::string> specific_character_cards;
};

static AiConfig globalAiConfig;

static std::map<ObjectGuid, std::string> conversationHistories;
static ObjectGuid currentConversationTarget;

struct NpcResponse { ObjectGuid npcGuid; uint32 mapId; uint32 instanceId; std::string text; };
struct StatusResponse { ObjectGuid playerGuid; bool isConnected; };

static std::queue<Player*> configRequestQueue;
static std::mutex configMutex;

static std::queue<NpcResponse> npcResponseQueue;
static std::mutex npcMutex;
static std::queue<StatusResponse> statusResponseQueue;
static std::mutex statusMutex;

//==============================================================================
// Event & Worker Structs
//==============================================================================
struct DelayedNpcSayEvent : public BasicEvent
{
    DelayedNpcSayEvent(Creature* creature, std::string const& text) : _creature(creature), _text(text) {}
    bool Execute(uint64, uint32) override
    {
        _creature->Say(_text, LANG_UNIVERSAL);
        _creature->HandleEmoteCommand(EMOTE_ONESHOT_TALK);
        return true;
    }
private:
    Creature* _creature;
    std::string _text;
};

struct NpcChatReactionWorker
{
    void operator()(Creature* creature, std::string const& text)
    {
        if (creature && creature->IsAlive())
        {
            creature->m_Events.AddEvent(new DelayedNpcSayEvent(creature, text), creature->m_Events.CalculateTime(50));
        }
    }
};

//==============================================================================
// Configuration Save/Load Functions
//==============================================================================
void SaveAIConfig()
{
    std::ofstream configFile("AI_Mod_Config.conf");
    if (configFile.is_open())
    {
        configFile << "host=" << globalAiConfig.host << std::endl;
        configFile << "port=" << globalAiConfig.port << std::endl;
        configFile << "max_context_length=" << globalAiConfig.max_context_length << std::endl;
        configFile << "max_length=" << globalAiConfig.max_length << std::endl;
        configFile << "temperature=" << globalAiConfig.temperature << std::endl;
        configFile << "repetition_penalty=" << globalAiConfig.repetition_penalty << std::endl;
        configFile << "top_p=" << globalAiConfig.top_p << std::endl;
        configFile << "top_k=" << globalAiConfig.top_k << std::endl;
        configFile.close();
        LOG_INFO("server", "[AI MANAGER] Configuration saved.");
    }
    else
    {
        LOG_ERROR("server", "[AI MANAGER] Could not write to AI_Mod_Config.conf.");
    }
}

void LoadAIConfig()
{
    std::ifstream configFile("AI_Mod_Config.conf");
    if (configFile.is_open())
    {
        std::string line;
        while (std::getline(configFile, line))
        {
            std::string::size_type separatorPos = line.find('=');
            if (separatorPos != std::string::npos)
            {
                std::string key = line.substr(0, separatorPos);
                std::string value = line.substr(separatorPos + 1);
                if (key == "host") globalAiConfig.host = value;
                else if (key == "port") globalAiConfig.port = std::stoi(value);
                else if (key == "max_context_length") globalAiConfig.max_context_length = std::stoi(value);
                else if (key == "max_length") globalAiConfig.max_length = std::stoi(value);
                else if (key == "temperature") globalAiConfig.temperature = std::stof(value);
                else if (key == "repetition_penalty") globalAiConfig.repetition_penalty = std::stof(value);
                else if (key == "top_p") globalAiConfig.top_p = std::stof(value);
                else if (key == "top_k") globalAiConfig.top_k = std::stoi(value);
            }
        }
        configFile.close();
        LOG_INFO("server", "[AI MANAGER] Configuration loaded.");
    }
    else
    {
        LOG_INFO("server", "[AI MANAGER] AI_Mod_Config.conf not found. Creating with defaults.");
        SaveAIConfig();
    }
    globalAiConfig.address = globalAiConfig.host + ":" + std::to_string(globalAiConfig.port);
}

//==============================================================================
// Addon Communication & Background Workers
//==============================================================================
void SendFullAIConfig(Player* player)
{
    if (!player) return;
    std::ostringstream configStream;
    configStream << std::fixed << std::setprecision(2);
    configStream << "host=" << globalAiConfig.host << ";"
        << "port=" << globalAiConfig.port << ";"
        << "max_context_length=" << globalAiConfig.max_context_length << ";"
        << "max_length=" << globalAiConfig.max_length << ";"
        << "temperature=" << globalAiConfig.temperature << ";"
        << "repetition_penalty=" << globalAiConfig.repetition_penalty << ";"
        << "top_p=" << globalAiConfig.top_p << ";"
        << "top_k=" << globalAiConfig.top_k << ";";

    std::string fullMessage = "[AIMgr_CONFIG]" + configStream.str();
    ChatHandler(player->GetSession()).PSendSysMessage(fullMessage.c_str());
}

void KoboldStatusCheckWorker(ObjectGuid playerGuid, std::string host, int port)
{
    bool isConnected = false;
    httplib::Client cli(host, port);
    cli.set_connection_timeout(2);
    if (auto res = cli.Get("/api/v1/model"))
        if (res->status == 200)
            isConnected = true;

    std::lock_guard<std::mutex> lock(statusMutex);
    statusResponseQueue.push({ playerGuid, isConnected });
}

void KoboldRequestWorker(ObjectGuid npcGuid, uint32 mapId, uint32 instanceId, std::string jsonData, std::string historyTurn)
{
    if (auto res = httplib::Client(globalAiConfig.host, globalAiConfig.port).Post("/api/v1/generate", jsonData, "application/json"))
    {
        if (res->status == 200)
        {
            auto jsonResponse = nlohmann::json::parse(res->body);
            std::string ai_text = jsonResponse["results"][0]["text"];
            ai_text.erase(0, ai_text.find_first_not_of(" \t\n\r"));
            ai_text.erase(ai_text.find_last_not_of(" \t\n\r") + 1);

            if (!ai_text.empty())
            {
                std::lock_guard<std::mutex> lock(npcMutex);
                npcResponseQueue.push({ npcGuid, mapId, instanceId, ai_text });
                conversationHistories[npcGuid] += historyTurn + " " + ai_text;
            }
        }
    }
}

//==============================================================================
// Player Script (Handles Chat Input)
//==============================================================================
class mod_kobold_npc_playerscript : public PlayerScript
{
public:
    mod_kobold_npc_playerscript() : PlayerScript("mod_kobold_npc_playerscript") {}

    void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& lang, std::string& msg) override
    {
        if (msg.find("AIMGR") != std::string::npos && msg.find("GET_CONFIG") != std::string::npos)
        {
            std::lock_guard<std::mutex> lock(configMutex);
            configRequestQueue.push(player);
            return;
        }
        else if (msg.find("AIMGR") != std::string::npos && msg.find("SAVE_CONFIG") != std::string::npos)
        {
            std::string data = msg.substr(msg.find("SAVE_CONFIG") + sizeof("SAVE_CONFIG"));

            std::string::size_type start = 0;
            while (start < data.length())
            {
                std::string::size_type end_key = data.find('=', start);
                if (end_key == std::string::npos) break;
                std::string key = data.substr(start, end_key - start);

                std::string::size_type end_val = data.find(';', end_key);
                if (end_val == std::string::npos) break;
                std::string value = data.substr(end_key + 1, end_val - (end_key + 1));

                if (key == "host") globalAiConfig.host = value;
                else if (key == "port") globalAiConfig.port = std::stoi(value);
                else if (key == "max_context_length") globalAiConfig.max_context_length = std::stoi(value);
                else if (key == "max_length") globalAiConfig.max_length = std::stoi(value);
                else if (key == "temperature") globalAiConfig.temperature = std::stof(value);
                else if (key == "repetition_penalty") globalAiConfig.repetition_penalty = std::stof(value);
                else if (key == "top_p") globalAiConfig.top_p = std::stof(value);
                else if (key == "top_k") globalAiConfig.top_k = std::stoi(value);

                start = end_val + 1;
            }

            globalAiConfig.address = globalAiConfig.host + ":" + std::to_string(globalAiConfig.port);
            SaveAIConfig();
            SendFullAIConfig(player);

            return;
        }

        if (type == CHAT_MSG_SAY)
        {
            Unit* target = player->GetSelectedUnit();
            if (target && target->ToCreature())
            {
                Creature* npcTarget = target->ToCreature();

                if (currentConversationTarget != npcTarget->GetGUID())
                {
                    if (currentConversationTarget)
                        conversationHistories.erase(currentConversationTarget);
                    currentConversationTarget = npcTarget->GetGUID();
                }

                std::vector<std::string> stopSequences;
                std::string sequence = globalAiConfig.stop_sequence;
                std::string delimiter = "||$||";
                size_t pos = 0;
                std::string token;
                while ((pos = sequence.find(delimiter)) != std::string::npos) {
                    token = sequence.substr(0, pos);
                    size_t n_pos = 0;
                    while ((n_pos = token.find("\\n", n_pos)) != std::string::npos) {
                        token.replace(n_pos, 2, "\n");
                    }
                    stopSequences.push_back(token);
                    sequence.erase(0, pos + delimiter.length());
                }
                size_t n_pos = 0;
                while ((n_pos = sequence.find("\\n", n_pos)) != std::string::npos) {
                    sequence.replace(n_pos, 2, "\n");
                }
                stopSequences.push_back(sequence);

                std::string currentCharacterCard = "";
                auto it = globalAiConfig.specific_character_cards.find(npcTarget->GetName());
                if (it != globalAiConfig.specific_character_cards.end())
                {
                    currentCharacterCard = it->second;
                }

                std::string& history = conversationHistories[npcTarget->GetGUID()];
                std::string current_turn = "\nPlayer: " + msg + "\n" + npcTarget->GetName() + ":";
                std::string full_prompt = globalAiConfig.system_prompt + "\n" + currentCharacterCard + history + current_turn;

                nlohmann::json data = {
                    {"prompt", full_prompt},
                    {"max_context_length", globalAiConfig.max_context_length},
                    {"max_length", globalAiConfig.max_length},
                    {"temperature", globalAiConfig.temperature},
                    {"top_p", globalAiConfig.top_p},
                    {"top_k", globalAiConfig.top_k},
                    {"rep_pen", globalAiConfig.repetition_penalty},
                    {"stop_sequence", stopSequences}
                };

                uint32 mapId = npcTarget->GetMapId();
                uint32 instanceId = npcTarget->GetInstanceId();

                std::thread(KoboldRequestWorker, npcTarget->GetGUID(), mapId, instanceId, data.dump(), current_turn).detach();
            }
            else
            {
                if (currentConversationTarget)
                {
                    conversationHistories.erase(currentConversationTarget);
                    currentConversationTarget = ObjectGuid::Empty;
                }
            }
        }
    }
};

//==============================================================================
// World Script (Handles Server Ticks & Startup)
//==============================================================================
class mod_kobold_npc_worldscript : public WorldScript
{
public:
    mod_kobold_npc_worldscript() : WorldScript("mod_kobold_npc_worldscript") {}

    void OnStartup() override
    {
        LoadAIConfig();
        LOG_INFO("server", "[AI MANAGER] Module loaded.");
    }

    void OnUpdate(uint32) override
    {
        if (!configRequestQueue.empty())
        {
            std::lock_guard<std::mutex> lock(configMutex);
            while (!configRequestQueue.empty())
            {
                Player* player = configRequestQueue.front();
                configRequestQueue.pop();
                if (player)
                {
                    SendFullAIConfig(player);
                    std::thread(KoboldStatusCheckWorker, player->GetGUID(), globalAiConfig.host, globalAiConfig.port).detach();
                }
            }
        }

        if (!statusResponseQueue.empty())
        {
            std::lock_guard<std::mutex> lock(statusMutex);
            while (!statusResponseQueue.empty())
            {
                StatusResponse res = statusResponseQueue.front();
                statusResponseQueue.pop();
                if (Player* player = ObjectAccessor::FindPlayer(res.playerGuid))
                {
                    std::string msg = "[AIMgr_STATUS]status=" + std::string(res.isConnected ? "true" : "false");
                    ChatHandler(player->GetSession()).PSendSysMessage(msg.c_str());
                }
            }
        }

        if (!npcResponseQueue.empty())
        {
            std::lock_guard<std::mutex> lock(npcMutex);
            while (!npcResponseQueue.empty())
            {
                NpcResponse res = npcResponseQueue.front();
                npcResponseQueue.pop();
                if (Map* map = sMapMgr->FindMap(res.mapId, res.instanceId))
                    if (Creature* npc = map->GetCreature(res.npcGuid))
                        NpcChatReactionWorker()(npc, res.text);
            }
        }
    }
};

//==============================================================================
// Module Loader
//==============================================================================
void Addkobold_npcScripts()
{
    new mod_kobold_npc_playerscript();
    new mod_kobold_npc_worldscript();
}
