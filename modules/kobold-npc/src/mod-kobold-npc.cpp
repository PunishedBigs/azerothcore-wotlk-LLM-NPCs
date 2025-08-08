#include "Player.h"
#include "ScriptMgr.h"
#include "Chat.h"
#include "Creature.h"
#include "ObjectGuid.h"
#include "MapMgr.h" // Required for sMapMgr
#include "Map.h"
#include <thread> // For running network requests in the background
#include <mutex>  // For thread-safe data access
#include <queue>  // For queuing NPC responses

// Includes for our mod's functionality
#include <fstream>
#include "json.hpp"
#include "httplib.h"


//==============================================================================
// Global AI Configuration & Structures
//==============================================================================

struct AiConfig
{
    // ... (All AiConfig members remain the same)
    std::string address = "127.0.0.1:5001";
    std::string host = "127.0.0.1";
    int port = 5001;
    int max_context_length = 8192;
    int max_length = 128;
    float temperature = 0.8f;
    float top_p = 0.9f;
    int top_k = 40;
    float repetition_penalty = 1.1f;
    std::string stop_sequence = "\\n||$||Player:||$||[INST]||$||</s>";
    std::string system_prompt = "You are a helpful AI assistant roleplaying as a character in the World of Warcraft.\nFollow these rules strictly:\n1. Always stay in character.\n2. Do not use newline characters in your response.\n3. Keep your responses to a single, concise paragraph.\n4. Never speak for the player.";
    std::map<std::string, std::string> specific_character_cards;
};

static AiConfig globalAiConfig;
static bool configLoaded = false;

// Conversation history management
static std::map<ObjectGuid, std::string> conversationHistories;
static ObjectGuid currentConversationTarget;

// Struct to hold the result from the background thread
struct NpcResponse
{
    ObjectGuid npcGuid;
    uint32 mapId;
    uint32 instanceId; // Added instanceId
    std::string text;
};

// Thread-safe queue to hold responses from the Kobold API
static std::queue<NpcResponse> npcResponseQueue;
static std::mutex queueMutex;

// Delayed event for NPC speech (remains the same)
struct DelayedNpcSayEvent : public BasicEvent
{
    DelayedNpcSayEvent(Creature* creature, std::string const& text)
        : _creature(creature), _text(text) {
    }

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

// Worker to trigger the delayed event (remains the same)
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
// Configuration Save/Load Functions (remain the same)
//==============================================================================
void SaveAIConfig() { /* ... function content ... */ }
void LoadAIConfig() { /* ... function content ... */ }

//==============================================================================
// Addon Communication Functions (remain the same)
//==============================================================================
void SendAIStatus(Player* player) { /* ... function content ... */ }
void SendFullAIConfig(Player* player) { /* ... function content ... */ }

//==============================================================================
// Background Worker Function
//==============================================================================
void KoboldRequestWorker(ObjectGuid npcGuid, uint32 mapId, uint32 instanceId, std::string jsonData, std::string historyTurn)
{
    // The jsonData is now passed directly
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
                // Safely add the response to the queue
                std::lock_guard<std::mutex> lock(queueMutex);
                npcResponseQueue.push({ npcGuid, mapId, instanceId, ai_text });

                // Also update the history here
                conversationHistories[npcGuid] += historyTurn + " " + ai_text;
            }
        }
        else
        {
            LOG_ERROR("server.chat", "KoboldCpp request failed with status: %d", res->status);
        }
    }
    else
    {
        LOG_ERROR("server.chat", "KoboldCpp request error: %s", httplib::to_string(res.error()).c_str());
    }
}


//==============================================================================
// Player Script Class (Handles Chat Input)
//==============================================================================
class mod_kobold_npc_playerscript : public PlayerScript
{
public:
    mod_kobold_npc_playerscript() : PlayerScript("mod_kobold_npc_playerscript") {}

    void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& lang, std::string& msg) override
    {
        // Addon handling remains the same
        if (lang == LANG_ADDON)
        {
            // ... addon message handling logic ...
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

                // CORRECTED: The JSON data is now fully constructed before being sent.
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

                // Get the map and instance ID to pass to the worker thread
                uint32 mapId = npcTarget->GetMapId();
                uint32 instanceId = npcTarget->GetInstanceId();

                // Launch the network request in a separate thread
                std::thread(KoboldRequestWorker, npcTarget->GetGUID(), mapId, instanceId, data.dump(), current_turn).detach();

                // By NOT modifying msg or type, and NOT returning early,
                // we allow the player's original chat message to appear instantly.
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
// World Script Class (Handles Server Ticks)
//==============================================================================
class mod_kobold_npc_worldscript : public WorldScript
{
public:
    mod_kobold_npc_worldscript() : WorldScript("mod_kobold_npc_worldscript") {}

    // This function runs on every server update tick
    void OnUpdate(uint32 /*diff*/) override
    {
        // Check if there are any responses waiting in the queue
        if (!npcResponseQueue.empty())
        {
            std::lock_guard<std::mutex> lock(queueMutex);

            // Process one response per tick to avoid lag spikes
            if (!npcResponseQueue.empty())
            {
                NpcResponse response = npcResponseQueue.front();
                npcResponseQueue.pop();

                // Find the map the creature is on, then get the creature from that map.
                if (Map* map = sMapMgr->FindMap(response.mapId, response.instanceId))
                {
                    if (Creature* npc = map->GetCreature(response.npcGuid))
                    {
                        NpcChatReactionWorker worker;
                        worker(npc, response.text);
                    }
                }
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
    new mod_kobold_npc_worldscript(); // Register the new world script
}


