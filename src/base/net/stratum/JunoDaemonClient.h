/* XMRig
 * Copyright (c) 2019      Howard Chu  <https://github.com/hyc>
 * Copyright (c) 2018-2021 SChernykh   <https://github.com/SChernykh>
 * Copyright (c) 2016-2021 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef XMRIG_JUNODAEMONCLIENT_H
#define XMRIG_JUNODAEMONCLIENT_H


#include "base/kernel/interfaces/IHttpListener.h"
#include "base/kernel/interfaces/ITimerListener.h"
#include "base/net/stratum/BaseClient.h"
#include "base/tools/Buffer.h"

#include <map>
#include <memory>
#include <vector>


namespace xmrig {


class Timer;


class JunoDaemonClient : public BaseClient, public ITimerListener, public IHttpListener
{
public:
    XMRIG_DISABLE_COPY_MOVE_DEFAULT(JunoDaemonClient)

    JunoDaemonClient(int id, IClientListener *listener);
    ~JunoDaemonClient() override;

protected:
    bool disconnect() override;
    bool isTLS() const override;
    int64_t submit(const JobResult &result) override;
    void connect() override;
    void connect(const Pool &pool) override;
    void setPool(const Pool &pool) override;

    void onHttpData(const HttpData &data) override;
    void onTimer(const Timer *timer) override;

    inline bool hasExtension(Extension) const noexcept override         { return false; }
    inline const char *mode() const override                            { return "juno-daemon"; }
    inline const char *tlsFingerprint() const override                  { return m_tlsFingerprint; }
    inline const char *tlsVersion() const override                      { return m_tlsVersion; }
    inline int64_t send(const rapidjson::Value &, Callback) override    { return -1; }
    inline int64_t send(const rapidjson::Value &) override              { return -1; }
    void deleteLater() override;
    inline void tick(uint64_t) override                                 {}

private:
    // RPC methods
    int64_t getBlockTemplate();
    int64_t rpcSend(const rapidjson::Document &doc);

    // Response parsing
    bool parseBlockTemplate(const rapidjson::Value &result);
    bool parseResponse(int64_t id, const rapidjson::Value &result, const rapidjson::Value &error);

    // Block construction
    String buildHashingBlob() const;
    String serializeBlock(const JobResult &result) const;

    // Target conversion (Bitcoin-style compact to difficulty)
    static uint64_t setCompact(uint32_t bits);

    // Utility
    static Buffer reverseBytes(const Buffer &input);
    static size_t writeVarint(uint8_t *buf, uint64_t value);

    void retry();
    void setState(SocketState state);

    // HTTP listener
    std::shared_ptr<IHttpListener> m_httpListener;
    Timer *m_timer = nullptr;
    String m_tlsFingerprint;
    String m_tlsVersion;

    // Block template data (Juno-specific 140-byte header)
    uint32_t m_version = 0;
    Buffer m_prevBlockHash;          // 32 bytes (internal order)
    Buffer m_merkleRoot;             // 32 bytes (internal order)
    Buffer m_blockCommitmentsHash;   // 32 bytes (internal order) - Juno-specific
    uint32_t m_curtime = 0;
    uint32_t m_bits = 0;
    uint64_t m_height = 0;
    uint64_t m_randomxSeedHeight = 0;
    Buffer m_randomxSeedHash;        // 32 bytes
    Buffer m_coinbaseTx;
    std::vector<Buffer> m_transactions;

    // Block template string for submission
    String m_blocktemplateStr;

    // Job tracking
    String m_currentJobId;
    String m_prevHash;
    uint64_t m_jobSteadyMs = 0;
    uint64_t m_blocktemplateRequestHeight = 0;
    String m_blocktemplateRequestHash;

    // Request type tracking
    enum RequestType {
        REQ_UNKNOWN,
        REQ_GETBLOCKTEMPLATE,
        REQ_SUBMITBLOCK
    };
    std::map<int64_t, RequestType> m_pendingRequests;
};


} /* namespace xmrig */


#endif /* XMRIG_JUNODAEMONCLIENT_H */
