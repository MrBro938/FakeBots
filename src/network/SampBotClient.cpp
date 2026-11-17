// ============================================================================
// FakeBots :: SampBotClient.cpp
// ============================================================================
#include "SampBotClient.h"

#include <SAMPRakNet.hpp>

#include <BitStream.h>
#include <NetworkTypes.h>
#include <PacketEnumerations.h>
#include <RakClientInterface.h>
#include <RakNetworkFactory.h>
#include <RakSleep.h>
#include <GetTime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

namespace FakeBots
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;

        const char* StateName(SampBotClient::State state)
        {
            switch (state)
            {
                case SampBotClient::State::Disconnected: return "Disconnected";
                case SampBotClient::State::Connecting: return "Connecting";
                case SampBotClient::State::AwaitingInit: return "AwaitingInit";
                case SampBotClient::State::AwaitingClassResponse: return "AwaitingClassResponse";
                case SampBotClient::State::AwaitingSpawnResponse: return "AwaitingSpawnResponse";
                case SampBotClient::State::Spawned: return "Spawned";
                case SampBotClient::State::Idle: return "Idle";
                case SampBotClient::State::Failed: return "Failed";
                default: return "Unknown";
            }
        }

        float Clamp100(float value)
        {
            if (!std::isfinite(value))
                return 0.0f;
            return std::max(0.0f, std::min(100.0f, value));
        }

        float Clamp1000(float value)
        {
            if (!std::isfinite(value))
                return 0.0f;
            return std::max(0.0f, std::min(1000.0f, value));
        }

        // The server drops sync packets whose position is outside these ranges.
        float ClampPos(float v, float lo, float hi)
        {
            if (!std::isfinite(v))
                return 0.0f;
            return std::max(lo, std::min(hi, v));
        }

        // ...and velocity vectors longer than 100 units.
        SampVector3 SanitizeVelocity(SampVector3 v)
        {
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
                return {};
            const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            if (len > 99.0f)
            {
                const float k = 99.0f / len;
                v.x *= k;
                v.y *= k;
                v.z *= k;
            }
            return v;
        }

        bool ReadVec3(RakNet::BitStream& bs, SampVector3& v)
        {
            return bs.Read(v.x) && bs.Read(v.y) && bs.Read(v.z);
        }

        bool IsValidPlayerId(int id)
        {
            return id >= 0 && id <= 1000;
        }

        size_t Utf8PrefixLength(const char* text, size_t maxBytes)
        {
            if (text == nullptr)
                return 0;
            const size_t full = std::strlen(text);
            if (full <= maxBytes)
                return full;
            size_t end = maxBytes;
            while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U)
                --end;
            return end;
        }

        // RakNet keeps a few process-wide, reference counted singletons (string compressor,
        // string table, RNG seed) that every RakPeer touches in its constructor/destructor
        // WITHOUT locking. Bots are created on the server thread and destroyed on the
        // disconnect worker, so those two moments are serialised here.
        std::recursive_mutex& RakLifecycleMutex()
        {
            static std::recursive_mutex m;
            return m;
        }

        // ---------------------------------------------------------------
        // Background disconnect worker.
        //
        // RakPeer::Disconnect() waits for the notification to be flushed and
        // the destructor joins the network thread (>= 15 ms each).  Doing that
        // for dozens of bots on the server thread would stall the whole
        // server, so the finished clients are handed to this worker instead.
        // ---------------------------------------------------------------
        class DisconnectWorker
        {
        public:
            static DisconnectWorker& Get()
            {
                static DisconnectWorker w;
                return w;
            }

            void Submit(RakNet::RakClientInterface* client, unsigned int blockMs)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_thread.joinable())
                    m_thread = std::thread(&DisconnectWorker::Run, this);
                m_queue.push_back({client, blockMs});
                m_pending.fetch_add(1);
                m_cv.notify_one();
            }

            void Flush()
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_idleCv.wait(lock, [this] { return m_pending.load() == 0; });
                m_stop = true;
                m_cv.notify_all();
                lock.unlock();
                if (m_thread.joinable())
                    m_thread.join();
                lock.lock();
                m_stop = false;
                m_thread = std::thread();
            }

            ~DisconnectWorker()
            {
                if (m_thread.joinable())
                {
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_stop = true;
                    }
                    m_cv.notify_all();
                    m_thread.join();
                }
            }

        private:
            struct Item
            {
                RakNet::RakClientInterface* client;
                unsigned int blockMs;
            };

            void Run()
            {
                for (;;)
                {
                    Item item{};
                    {
                        std::unique_lock<std::mutex> lock(m_mutex);
                        m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
                        if (m_queue.empty())
                            return; // m_stop
                        item = m_queue.front();
                        m_queue.pop_front();
                    }
                    item.client->Disconnect(item.blockMs);
                    {
                        std::lock_guard<std::recursive_mutex> lifecycle(RakLifecycleMutex());
                        RakNet::RakNetworkFactory::DestroyRakClientInterface(item.client);
                    }
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_pending.fetch_sub(1);
                    }
                    m_idleCv.notify_all();
                }
            }

            std::mutex m_mutex;
            std::condition_variable m_cv;
            std::condition_variable m_idleCv;
            std::deque<Item> m_queue;
            std::atomic<int> m_pending{0};
            std::thread m_thread;
            bool m_stop = false;
        };
    }

    // ------------------------------------------------------------------
    //  Helpers
    // ------------------------------------------------------------------
    uint32_t SampBotClient::NowMs()
    {
        using namespace std::chrono;
        return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }

    float SampBotClient::ClampAngle(float degrees)
    {
        if (!std::isfinite(degrees))
            return 0.0f;
        degrees = std::fmod(degrees, 360.0f);
        if (degrees < 0.0f)
            degrees += 360.0f;
        return degrees;
    }

    // Rotation about the Z axis in GTA's convention (identical to the server's
    // GTAQuat(0, 0, angle)): w = cos(a/2), z = -sin(a/2).
    void SampBotClient::FacingToQuaternion(float facingDegrees, float (&wxyz)[4])
    {
        const float half = ClampAngle(facingDegrees) * kPi / 180.0f * -0.5f;
        wxyz[0] = std::cos(half);
        wxyz[1] = 0.0f;
        wxyz[2] = 0.0f;
        wxyz[3] = std::sin(half);
    }

    // Wire order: w, x, y, z (the open.mp SDK builds with GLM_FORCE_QUAT_DATA_WXYZ, which
    // is the layout of the original SA-MP structures).
    void SampBotClient::WriteQuaternion(RakNet::BitStream& bs, const float (&wxyz)[4])
    {
        bs.Write(wxyz[0]); // w
        bs.Write(wxyz[1]); // x
        bs.Write(wxyz[2]); // y
        bs.Write(wxyz[3]); // z
    }

    // A "gpci" value: the server only accepts hexadecimal serials that are
    // divisible by 1001 (that is how genuine clients generate them).
    std::string SampBotClient::GenerateSerial()
    {
        static std::mutex rngMutex;
        static std::mt19937_64 rng{[]
        {
            std::random_device rd;
            return (static_cast<uint64_t>(rd()) << 32U) ^ rd() ^
                   static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        }()};

        // 37 random hex digits, first one non-zero
        int digits[48] = {};
        const int n = 37;
        {
            std::lock_guard<std::mutex> lock(rngMutex);
            for (int i = 0; i < n; ++i)
                digits[i] = static_cast<int>(rng() & 0xF);
        }
        if (digits[0] == 0)
            digits[0] = 1;

        // multiply the big hex number by 1001 (little-endian accumulate)
        std::string out;
        unsigned carry = 0;
        for (int i = n - 1; i >= 0; --i)
        {
            const unsigned v = static_cast<unsigned>(digits[i]) * 1001u + carry;
            out.push_back("0123456789ABCDEF"[v & 0xF]);
            carry = v >> 4;
        }
        while (carry)
        {
            out.push_back("0123456789ABCDEF"[carry & 0xF]);
            carry >>= 4;
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

    // ------------------------------------------------------------------
    //  Lifecycle
    // ------------------------------------------------------------------
    SampBotClient::SampBotClient(std::string name)
        : m_name(std::move(name))
    {
    }

    SampBotClient::~SampBotClient()
    {
        Stop(0);
    }

    void SampBotClient::FlushBackgroundDisconnects()
    {
        DisconnectWorker::Get().Flush();
    }

    void SampBotClient::EnterState(State state)
    {
        if (m_state != state)
            ((void)0);
        m_state = state;
    }

    bool SampBotClient::Start(const Config& config)
    {
        Stop(0);

        if (config.host.empty() || config.port == 0 || m_name.empty())
        {
            sampgdk::logprintf("[FakeBots][START] rejected invalid config name='%s' host='%s' port=%u.",
                m_name.c_str(), config.host.c_str(), static_cast<unsigned>(config.port));
            m_state = State::Failed;
            return false;
        }

        m_config = config;
        if (m_config.authKey.empty())
            m_config.authKey = GenerateSerial();

        m_playerId = kInvalidId;
        m_failureNotified = false;
        m_dead = false;
        m_playerIdAnnounced = false;
        m_hasSynced = false;
        m_onFootRateMs = std::clamp(m_config.defaultOnFootRateMs, kMinSyncIntervalMs, kMaxSyncIntervalMs);
        m_inCarRateMs = std::clamp(m_config.defaultInCarRateMs, kMinSyncIntervalMs, kMaxSyncIntervalMs);
        m_lastOnFootSyncMs = 0;
        m_lastInCarSyncMs = 0;
        m_lastAnySyncMs = 0;
        m_local = SampBotLocalState{};

        std::lock_guard<std::recursive_mutex> lifecycle(RakLifecycleMutex());
        RakNet::GetTime(); // initialise RakNet's clock once, on this thread

        m_client = RakNet::RakNetworkFactory::GetRakClientInterface();
        if (m_client == nullptr)
        {
            ((void)0);
            m_state = State::Failed;
            return false;
        }

        m_client->SetPassword(m_config.password.empty() ? nullptr : m_config.password.c_str());
        RegisterRpcHandlers();
        m_client->StopOccasionalPing();

        // Connection attempts: one request per second until the limit below.
        const uint32_t seconds = std::max<uint32_t>(3, m_config.connectTimeoutMs / 1000);
        SAMPRakNet::SetMaxConnectionAttempts(seconds);

        ((void)0);
