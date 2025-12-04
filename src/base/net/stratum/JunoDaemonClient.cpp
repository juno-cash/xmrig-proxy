/* XMRig
 * Copyright (c) 2019      Howard Chu  <https://github.com/hyc>
 * Copyright (c) 2018-2023 SChernykh   <https://github.com/SChernykh>
 * Copyright (c) 2016-2023 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
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


#include "base/net/stratum/JunoDaemonClient.h"
#include "3rdparty/rapidjson/document.h"
#include "3rdparty/rapidjson/error/en.h"
#include "base/io/json/Json.h"
#include "base/io/json/JsonRequest.h"
#include "base/io/log/Log.h"
#include "base/kernel/interfaces/IClientListener.h"
#include "base/net/http/Fetch.h"
#include "base/net/http/HttpData.h"
#include "base/net/http/HttpListener.h"
#include "base/net/stratum/SubmitResult.h"
#include "base/tools/Chrono.h"
#include "base/tools/Cvt.h"
#include "base/tools/Timer.h"
#include "net/JobResult.h"

#include <algorithm>
#include <cassert>
#include <cstring>


namespace xmrig {

// Juno Cash epoch constants
static constexpr uint64_t kEpochLength = 2048;
static constexpr uint64_t kSeedLag = 96;

// Juno block header constants
static constexpr size_t kHeaderSize = 140;
static constexpr size_t kNonceOffset = 108;
static constexpr size_t kNonceSize = 32;
static constexpr size_t kSolutionSize = 32;

static const char *kJsonRPC = "/";

} // namespace xmrig


xmrig::JunoDaemonClient::JunoDaemonClient(int id, IClientListener *listener) :
    BaseClient(id, listener)
{
    m_httpListener = std::make_shared<HttpListener>(this);
    m_timer = new Timer(this);
}


xmrig::JunoDaemonClient::~JunoDaemonClient()
{
    delete m_timer;
}


void xmrig::JunoDaemonClient::deleteLater()
{
    delete this;
}


bool xmrig::JunoDaemonClient::disconnect()
{
    if (m_state != UnconnectedState) {
        setState(UnconnectedState);
    }
    return true;
}


bool xmrig::JunoDaemonClient::isTLS() const
{
#   ifdef XMRIG_FEATURE_TLS
    return m_pool.isTLS();
#   else
    return false;
#   endif
}


int64_t xmrig::JunoDaemonClient::submit(const JobResult &result)
{
    if (result.jobId != m_currentJobId) {
        return -1;
    }

    const String blockHex = serializeBlock(result);

    using namespace rapidjson;
    Document doc(kObjectType);
    auto &allocator = doc.GetAllocator();

    // JSON-RPC 1.0 format for submitblock
    doc.AddMember("id", m_sequence, allocator);
    doc.AddMember("method", "submitblock", allocator);

    Value params(kArrayType);
    params.PushBack(blockHex.toJSON(), allocator);
    doc.AddMember("params", params, allocator);

    m_pendingRequests[m_sequence] = REQ_SUBMITBLOCK;

#   ifdef XMRIG_PROXY_PROJECT
    m_results[m_sequence] = SubmitResult(m_sequence, result.diff, result.actualDiff(), result.id, 0);
#   else
    m_results[m_sequence] = SubmitResult(m_sequence, result.diff, result.actualDiff(), 0, result.backend);
#   endif

    return rpcSend(doc);
}


void xmrig::JunoDaemonClient::connect()
{
    auto connectError = [this](const char *message) {
        if (!isQuiet()) {
            LOG_ERR("%s " RED("connect error: ") RED_BOLD("\"%s\""), tag(), message);
        }
        retry();
    };

    setState(ConnectingState);

    if (!m_pool.algorithm().isValid()) {
        m_pool.setAlgo(Algorithm::RX_0);
    }

    getBlockTemplate();
}


void xmrig::JunoDaemonClient::connect(const Pool &pool)
{
    setPool(pool);
    connect();
}


void xmrig::JunoDaemonClient::setPool(const Pool &pool)
{
    BaseClient::setPool(pool);
}


void xmrig::JunoDaemonClient::onHttpData(const HttpData &data)
{
    if (data.status == 401) {
        if (!isQuiet()) {
            LOG_ERR("%s " RED("authentication failed"), tag());
        }
        return retry();
    }

    if (data.status != 200) {
        if (!isQuiet()) {
            LOG_ERR("%s " RED("HTTP error %d"), tag(), data.status);
        }
        return retry();
    }

    m_ip = data.ip().c_str();

#   ifdef XMRIG_FEATURE_TLS
    m_tlsVersion = data.tlsVersion();
    m_tlsFingerprint = data.tlsFingerprint();
#   endif

    rapidjson::Document doc;
    if (doc.Parse(data.body.c_str()).HasParseError()) {
        if (!isQuiet()) {
            LOG_ERR("%s " RED("JSON decode failed: ") RED_BOLD("\"%s\""), tag(), rapidjson::GetParseError_En(doc.GetParseError()));
        }
        return retry();
    }

    if (!parseResponse(Json::getInt64(doc, "id", -1), Json::getObject(doc, "result"), Json::getValue(doc, "error"))) {
        retry();
    }
}


void xmrig::JunoDaemonClient::onTimer(const Timer *)
{
    if (Chrono::steadyMSecs() >= m_jobSteadyMs + m_pool.jobTimeout()) {
        m_prevHash = nullptr;
        m_blocktemplateRequestHash = nullptr;
    }

    if (m_state == ConnectingState) {
        connect();
    }
    else if (m_state == ConnectedState) {
        getBlockTemplate();
    }
}


int64_t xmrig::JunoDaemonClient::getBlockTemplate()
{
    using namespace rapidjson;
    Document doc(kObjectType);
    auto &allocator = doc.GetAllocator();

    // JSON-RPC 1.0 format (no "jsonrpc" field)
    doc.AddMember("id", m_sequence, allocator);
    doc.AddMember("method", "getblocktemplate", allocator);

    // Juno's getblocktemplate params
    Value params(kObjectType);
    Value capabilities(kArrayType);
    capabilities.PushBack("coinbasetxn", allocator);
    capabilities.PushBack("workid", allocator);
    capabilities.PushBack("coinbase/append", allocator);
    params.AddMember("capabilities", capabilities, allocator);

    doc.AddMember("params", params, allocator);

    m_pendingRequests[m_sequence] = REQ_GETBLOCKTEMPLATE;

    return rpcSend(doc);
}


int64_t xmrig::JunoDaemonClient::rpcSend(const rapidjson::Document &doc)
{
    FetchRequest req(HTTP_POST, m_pool.host(), m_pool.port(), kJsonRPC, doc, m_pool.isTLS(), isQuiet());

    // Add HTTP Basic Auth if credentials provided
    if (!m_pool.user().isEmpty()) {
        std::string credentials = m_pool.user().data();
        credentials += ":";
        if (!m_pool.password().isEmpty()) {
            credentials += m_pool.password().data();
        }

        // Base64 encode credentials
        static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string encoded;
        int val = 0, valb = -6;
        for (unsigned char c : credentials) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                encoded.push_back(base64_chars[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        if (valb > -6) {
            encoded.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
        }
        while (encoded.size() % 4) {
            encoded.push_back('=');
        }

        req.headers.insert({"Authorization", "Basic " + encoded});
    }

    fetch(tag(), std::move(req), m_httpListener);

    return m_sequence++;
}


bool xmrig::JunoDaemonClient::parseBlockTemplate(const rapidjson::Value &result)
{
    auto templateError = [this](const char *message) {
        if (!isQuiet()) {
            LOG_ERR("%s " RED("block template error: ") RED_BOLD("\"%s\""), tag(), message);
        }
        return false;
    };

    // Parse Juno block template fields
    m_version = Json::getUint(result, "version");
    m_curtime = Json::getUint(result, "curtime");
    m_height = Json::getUint64(result, "height");
    m_randomxSeedHeight = Json::getUint64(result, "randomxseedheight");

    // Parse bits (compact difficulty target)
    const char *bitsStr = Json::getString(result, "bits");
    if (!bitsStr) {
        return templateError("Missing 'bits' field");
    }
    m_bits = static_cast<uint32_t>(strtoul(bitsStr, nullptr, 16));

    // Parse previous block hash (display order, needs reversal)
    const char *prevHashStr = Json::getString(result, "previousblockhash");
    if (!prevHashStr || strlen(prevHashStr) != 64) {
        return templateError("Invalid previousblockhash");
    }
    Buffer prevHashDisplay = Cvt::fromHex(prevHashStr, 64);
    m_prevBlockHash = reverseBytes(prevHashDisplay);

    // Parse randomx seed hash (internal order, no reversal)
    const char *seedHashStr = Json::getString(result, "randomxseedhash");
    if (!seedHashStr || strlen(seedHashStr) != 64) {
        return templateError("Invalid randomxseedhash");
    }
    m_randomxSeedHash = Cvt::fromHex(seedHashStr, 64);

    // Parse defaultroots object
    const rapidjson::Value &roots = Json::getObject(result, "defaultroots");
    if (roots.IsNull()) {
        return templateError("Missing 'defaultroots' object");
    }

    // Merkle root (display order, needs reversal)
    const char *merkleRootStr = Json::getString(roots, "merkleroot");
    if (!merkleRootStr || strlen(merkleRootStr) != 64) {
        return templateError("Invalid merkleroot");
    }
    Buffer merkleRootDisplay = Cvt::fromHex(merkleRootStr, 64);
    m_merkleRoot = reverseBytes(merkleRootDisplay);

    // Block commitments hash (display order, needs reversal) - Juno-specific
    const char *commitmentsStr = Json::getString(roots, "blockcommitmentshash");
    if (!commitmentsStr || strlen(commitmentsStr) != 64) {
        return templateError("Invalid blockcommitmentshash");
    }
    Buffer commitmentsDisplay = Cvt::fromHex(commitmentsStr, 64);
    m_blockCommitmentsHash = reverseBytes(commitmentsDisplay);

    // Parse coinbase transaction
    const rapidjson::Value &coinbaseTxn = Json::getObject(result, "coinbasetxn");
    if (coinbaseTxn.IsNull()) {
        return templateError("Missing 'coinbasetxn' object");
    }
    const char *coinbaseData = Json::getString(coinbaseTxn, "data");
    if (!coinbaseData) {
        return templateError("Missing coinbasetxn data");
    }
    m_coinbaseTx = Cvt::fromHex(coinbaseData, strlen(coinbaseData));

    // Parse other transactions
    m_transactions.clear();
    const rapidjson::Value &txArray = Json::getArray(result, "transactions");
    if (txArray.IsArray()) {
        for (const auto &tx : txArray.GetArray()) {
            const char *txData = Json::getString(tx, "data");
            if (txData) {
                m_transactions.push_back(Cvt::fromHex(txData, strlen(txData)));
            }
        }
    }

    // Build the hashing blob (140-byte header)
    String hashingBlob = buildHashingBlob();

    // Store the blocktemplate string for later serialization
    m_blocktemplateStr = hashingBlob;

    // Create Job
    Job job(false, Algorithm::RX_0, String());
    job.setHeight(m_height);

    // Calculate difficulty from compact bits
    uint64_t target = setCompact(m_bits);
    if (target == 0) {
        return templateError("Invalid difficulty target");
    }
    uint64_t diff = 0xFFFFFFFFFFFFFFFFULL / target;
    job.setDiff(diff);

    // Set seed hash for RandomX
    job.setSeedHash(Cvt::toHex(m_randomxSeedHash));

    // Set the hashing blob
    if (!job.setBlob(hashingBlob)) {
        return templateError("Failed to set blob");
    }

    // Generate job ID
    m_currentJobId = Cvt::toHex(Cvt::randomBytes(4));
    job.setId(m_currentJobId);

    // Store job
    m_job = std::move(job);
    m_prevHash = prevHashStr;
    m_jobSteadyMs = Chrono::steadyMSecs();

    if (m_state == ConnectingState) {
        setState(ConnectedState);
    }

    m_listener->onJobReceived(this, m_job, result);
    return true;
}


bool xmrig::JunoDaemonClient::parseResponse(int64_t id, const rapidjson::Value &result, const rapidjson::Value &error)
{
    if (id == -1) {
        return false;
    }

    auto it = m_pendingRequests.find(id);
    RequestType reqType = (it != m_pendingRequests.end()) ? it->second : REQ_UNKNOWN;
    if (it != m_pendingRequests.end()) {
        m_pendingRequests.erase(it);
    }

    // Check for JSON-RPC error
    if (!error.IsNull()) {
        const char *message = "Unknown error";
        if (error.IsObject()) {
            message = Json::getString(error, "message", "Unknown error");
        } else if (error.IsString()) {
            message = error.GetString();
        }

        if (reqType == REQ_SUBMITBLOCK) {
            if (!handleSubmitResponse(id, message)) {
                if (!isQuiet()) {
                    LOG_ERR("[%s:%d] error: " RED_BOLD("\"%s\""), m_pool.host().data(), m_pool.port(), message);
                }
            }
            // Get new template after submit failure
            getBlockTemplate();
            return true;
        }

        if (!isQuiet()) {
            LOG_ERR("[%s:%d] error: " RED_BOLD("\"%s\""), m_pool.host().data(), m_pool.port(), message);
        }
        return false;
    }

    switch (reqType) {
    case REQ_GETBLOCKTEMPLATE:
        if (result.IsObject()) {
            return parseBlockTemplate(result);
        }
        break;

    case REQ_SUBMITBLOCK:
        {
            // submitblock returns null on success, or a status string
            const char *status = nullptr;
            if (result.IsNull()) {
                status = nullptr;  // Success
            } else if (result.IsString()) {
                status = result.GetString();
                // "duplicate", "inconclusive", "duplicate-inconclusive" are acceptable
                if (strcmp(status, "duplicate") != 0 &&
                    strcmp(status, "inconclusive") != 0 &&
                    strcmp(status, "duplicate-inconclusive") != 0) {
                    // Rejection
                    if (!handleSubmitResponse(id, status) && !isQuiet()) {
                        LOG_ERR("[%s:%d] block rejected: " RED_BOLD("\"%s\""), m_pool.host().data(), m_pool.port(), status);
                    }
                    getBlockTemplate();
                    return true;
                }
            }

            // Success
            handleSubmitResponse(id, nullptr);
            getBlockTemplate();
            return true;
        }
        break;

    default:
        break;
    }

    return false;
}


xmrig::String xmrig::JunoDaemonClient::buildHashingBlob() const
{
    // Build 140-byte Juno block header
    // Layout:
    // [0-3]     nVersion (4 bytes LE)
    // [4-35]    hashPrevBlock (32 bytes)
    // [36-67]   hashMerkleRoot (32 bytes)
    // [68-99]   hashBlockCommitments (32 bytes)
    // [100-103] nTime (4 bytes LE)
    // [104-107] nBits (4 bytes LE)
    // [108-139] nNonce (32 bytes) - filled with zeros, to be set by miner

    uint8_t header[kHeaderSize] = {0};

    // Version (LE32)
    header[0] = m_version & 0xFF;
    header[1] = (m_version >> 8) & 0xFF;
    header[2] = (m_version >> 16) & 0xFF;
    header[3] = (m_version >> 24) & 0xFF;

    // Previous block hash (already in internal order)
    if (m_prevBlockHash.size() == 32) {
        memcpy(header + 4, m_prevBlockHash.data(), 32);
    }

    // Merkle root (already in internal order)
    if (m_merkleRoot.size() == 32) {
        memcpy(header + 36, m_merkleRoot.data(), 32);
    }

    // Block commitments hash (already in internal order)
    if (m_blockCommitmentsHash.size() == 32) {
        memcpy(header + 68, m_blockCommitmentsHash.data(), 32);
    }

    // Time (LE32)
    header[100] = m_curtime & 0xFF;
    header[101] = (m_curtime >> 8) & 0xFF;
    header[102] = (m_curtime >> 16) & 0xFF;
    header[103] = (m_curtime >> 24) & 0xFF;

    // Bits (LE32)
    header[104] = m_bits & 0xFF;
    header[105] = (m_bits >> 8) & 0xFF;
    header[106] = (m_bits >> 16) & 0xFF;
    header[107] = (m_bits >> 24) & 0xFF;

    // Nonce is left as zeros (32 bytes at offset 108)
    // The miner will fill this in

    return Cvt::toHex(header, kHeaderSize);
}


xmrig::String xmrig::JunoDaemonClient::serializeBlock(const JobResult &result) const
{
    // Serialize full block for submitblock RPC
    // Format:
    // - Header (140 bytes) with nonce filled in
    // - Solution size varint (0x20 = 32 bytes)
    // - Solution (32 bytes) - the RandomX hash result
    // - Transaction count varint
    // - Coinbase transaction
    // - Other transactions

    std::vector<uint8_t> block;
    block.reserve(1024 + m_coinbaseTx.size());

    // Build header with nonce from result
    uint8_t header[kHeaderSize];

    // Copy the base header (version through bits)
    header[0] = m_version & 0xFF;
    header[1] = (m_version >> 8) & 0xFF;
    header[2] = (m_version >> 16) & 0xFF;
    header[3] = (m_version >> 24) & 0xFF;

    if (m_prevBlockHash.size() == 32) {
        memcpy(header + 4, m_prevBlockHash.data(), 32);
    }
    if (m_merkleRoot.size() == 32) {
        memcpy(header + 36, m_merkleRoot.data(), 32);
    }
    if (m_blockCommitmentsHash.size() == 32) {
        memcpy(header + 68, m_blockCommitmentsHash.data(), 32);
    }

    header[100] = m_curtime & 0xFF;
    header[101] = (m_curtime >> 8) & 0xFF;
    header[102] = (m_curtime >> 16) & 0xFF;
    header[103] = (m_curtime >> 24) & 0xFF;

    header[104] = m_bits & 0xFF;
    header[105] = (m_bits >> 8) & 0xFF;
    header[106] = (m_bits >> 16) & 0xFF;
    header[107] = (m_bits >> 24) & 0xFF;

#   ifdef XMRIG_PROXY_PROJECT
    // In proxy mode, the nonce comes from result.nonce as hex string (8 chars = 4 bytes)
    // But for Juno, we need the full 32-byte nonce from the blob
    // The proxy receives the modified blob back from the miner with the nonce filled in
    // We need to extract the nonce from positions 108-139 (32 bytes = 64 hex chars)

    // Get the nonce from the result - for proxy it's in the nonce field as hex
    // Copy 4 bytes from result.nonce (which is 8 hex chars for regular coins)
    // For Juno with 32-byte nonce, we need different handling

    // The miner sends back the nonce portion - let's assume the first 4 bytes are in result.nonce
    // and we need to reconstruct the full 32-byte nonce
    memset(header + 108, 0, 32);

    // Copy the 4-byte nonce that the proxy received
    if (result.nonce) {
        Cvt::fromHex(header + 108, 4, result.nonce, 8);
    }
#   else
    // Get the 32-byte nonce from the job blob
    const uint8_t *blob = m_job.blob();
    memcpy(header + 108, blob + 108, 32);
#   endif

    // Add header to block
    block.insert(block.end(), header, header + kHeaderSize);

    // Add solution size as varint (32 = 0x20)
    block.push_back(0x20);

    // Add solution (32 bytes) - the RandomX hash result
    // In proxy mode, result.result is a hex string (64 chars = 32 bytes)
    uint8_t solution[32];
    Cvt::fromHex(solution, 32, result.result, 64);
    block.insert(block.end(), solution, solution + 32);

    // Add transaction count as varint
    uint64_t txCount = 1 + m_transactions.size();
    uint8_t varintBuf[9];
    size_t varintLen = writeVarint(varintBuf, txCount);
    block.insert(block.end(), varintBuf, varintBuf + varintLen);

    // Add coinbase transaction
    block.insert(block.end(), m_coinbaseTx.data(), m_coinbaseTx.data() + m_coinbaseTx.size());

    // Add other transactions
    for (const auto &tx : m_transactions) {
        block.insert(block.end(), tx.data(), tx.data() + tx.size());
    }

    return Cvt::toHex(block.data(), block.size());
}


uint64_t xmrig::JunoDaemonClient::setCompact(uint32_t bits)
{
    // Convert Bitcoin-style compact target representation to 64-bit target
    // Format: bits = [exponent (8 bits)] [mantissa (24 bits)]
    // Target = mantissa * 2^(8 * (exponent - 3))

    uint32_t exponent = bits >> 24;
    uint32_t mantissa = bits & 0x007FFFFF;

    // Handle negative flag (shouldn't happen for valid targets)
    if (bits & 0x00800000) {
        return 0;
    }

    if (exponent == 0) {
        return 0;
    }

    // Calculate target as 64-bit value (we only need the high bits for comparison)
    // For mining, we compare hash < target, so we need the actual target value
    // Since target can be > 64 bits, we calculate a 64-bit approximation

    uint64_t target = 0;

    if (exponent <= 3) {
        target = mantissa >> (8 * (3 - exponent));
    } else if (exponent <= 11) {
        target = static_cast<uint64_t>(mantissa) << (8 * (exponent - 3));
    } else {
        // Target is > 64 bits, return max value
        return 0xFFFFFFFFFFFFFFFFULL;
    }

    return target;
}


xmrig::Buffer xmrig::JunoDaemonClient::reverseBytes(const Buffer &input)
{
    Buffer output(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        output.data()[i] = input.data()[input.size() - 1 - i];
    }
    return output;
}


size_t xmrig::JunoDaemonClient::writeVarint(uint8_t *buf, uint64_t value)
{
    if (value < 0xFD) {
        buf[0] = static_cast<uint8_t>(value);
        return 1;
    } else if (value <= 0xFFFF) {
        buf[0] = 0xFD;
        buf[1] = value & 0xFF;
        buf[2] = (value >> 8) & 0xFF;
        return 3;
    } else if (value <= 0xFFFFFFFF) {
        buf[0] = 0xFE;
        buf[1] = value & 0xFF;
        buf[2] = (value >> 8) & 0xFF;
        buf[3] = (value >> 16) & 0xFF;
        buf[4] = (value >> 24) & 0xFF;
        return 5;
    } else {
        buf[0] = 0xFF;
        for (int i = 0; i < 8; ++i) {
            buf[1 + i] = (value >> (8 * i)) & 0xFF;
        }
        return 9;
    }
}


void xmrig::JunoDaemonClient::retry()
{
    m_failures++;
    m_listener->onClose(this, static_cast<int>(m_failures));

    if (m_failures == -1) {
        return;
    }

    if (m_state == ConnectedState) {
        setState(ConnectingState);
    }

    m_timer->stop();
    m_timer->start(m_retryPause, 0);
}


void xmrig::JunoDaemonClient::setState(SocketState state)
{
    if (m_state == state) {
        return;
    }

    m_state = state;

    switch (state) {
    case ConnectedState:
        {
            m_failures = 0;
            m_listener->onLoginSuccess(this);

            const uint64_t interval = std::max<uint64_t>(20, m_pool.pollInterval());
            m_timer->start(interval, interval);
        }
        break;

    case UnconnectedState:
        m_failures = -1;
        m_timer->stop();
        break;

    default:
        break;
    }
}
